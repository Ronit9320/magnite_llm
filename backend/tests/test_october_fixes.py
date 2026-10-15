"""Assertions for BUG-001 through BUG-013 in BUG_REPORT.md."""
import asyncio
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import zipfile

import numpy as np

import test_regressions as fixtures
from backend.routes import adapters, rag, training
from backend.training.datasets import DatasetStore, CSV_FIELD_LIMIT
from backend.training.jobs import TrainingJobManager
from backend.training.schemas import TrainingConfig
from backend.training.trainer import _resolve_base
from backend.rag.chunking import extract_content, extract_text, chunk_text


class OctoberApiTests(unittest.IsolatedAsyncioTestCase):
    asyncSetUp = fixtures.ApiTests.asyncSetUp
    asyncTearDown = fixtures.ApiTests.asyncTearDown
    archive = fixtures.ApiTests.archive

    async def test_export_urls_are_unique_and_immutable(self):
        urls = []
        for identifier in ('adapter:collision', 'adapter-collision', 'adapter:collision'):
            if identifier != 'adapter:collision' or not urls:
                path = self.archive(record=dict(id=identifier, name=identifier, base_model='test', path=''))
                self.assertEqual((await self.client.post('/adapters/import', json={'path': str(path)})).status_code, 201)
            result = await self.client.post('/adapters/export/' + identifier)
            self.assertEqual(result.status_code, 200, result.text)
            urls.append(result.json()['download_path'])
        self.assertEqual(len(set(urls)), 3)
        for url, expected in zip(urls, ('adapter:collision', 'adapter-collision', 'adapter:collision')):
            import io
            response = await self.client.get(url)
            with zipfile.ZipFile(io.BytesIO(response.content)) as archive:
                self.assertEqual(json.loads(archive.read('record.json'))['id'], expected)
        results = await asyncio.gather(
            self.client.get(urls[0]),
            self.client.post('/adapters/export/adapter:collision'),
            self.client.post('/adapters/export/adapter:collision'))
        self.assertTrue(all(result.status_code == 200 for result in results))
        self.assertNotEqual(results[1].json()['download_path'], results[2].json()['download_path'])
        self.assertEqual(results[0].content, (await self.client.get(urls[0])).content)
        rec = adapters._get_store().get('adapter:collision')
        rec.path = str(self.root/'missing')
        adapters._get_store().upsert(rec)
        self.assertEqual((await self.client.post('/adapters/export/'+rec.id)).status_code, 400)
        self.assertEqual((await self.client.get(urls[0])).status_code, 200)

    async def test_corrupt_weights_and_incomplete_config_are_rejected(self):
        for corrupt_config in (False, True):
            path = self.archive()
            with zipfile.ZipFile(path) as z:
                contents = {n: z.read(n) for n in z.namelist()}
            contents['weights/adapter_config.json' if corrupt_config else 'weights/adapter_model.safetensors'] = (
                b'{"peft_type":"LORA"}' if corrupt_config else b'not a tensor')
            with zipfile.ZipFile(path, 'w') as z:
                for name, data in contents.items(): z.writestr(name, data)
            response = await self.client.post('/adapters/import', json={'path': str(path)})
            self.assertEqual(response.status_code, 400, response.text)
            self.assertEqual(adapters._get_store().list(), [])
            self.assertEqual(list((self.root/'adapters/imports').iterdir()), [])

    async def test_model_mismatch_chat_and_completion(self):
        for endpoint, body in [('chat', {'messages':[{'role':'user','content':'hi'}]}), ('completion', {'prompt':'hi'})]:
            for stream in (False, True):
                response = await self.client.post('/inference/'+endpoint, json={**body, 'model':'different', 'stream':stream})
                self.assertEqual(response.status_code, 400, response.text)
                response = await self.client.post('/inference/'+endpoint, json={**body, 'model':'test', 'stream':stream})
                self.assertEqual(response.status_code, 200, response.text)

    async def test_registered_snapshot_is_used_by_training_and_activation(self):
        from backend.routes import models
        from backend.models.schemas import ModelRecord
        from backend.adapters.schemas import AdapterRecord
        path = self.root/'snapshot'
        path.mkdir()
        record = ModelRecord(id='huggingface:owner--model', name='x', source='huggingface', ref='owner/model', path=str(path))
        models._get_registry()._store.upsert(record)
        self.assertEqual(_resolve_base(record.id), str(path))
        rec = AdapterRecord(id='a', name='a', base_model=record.id, path=str(self.root))
        adapters._get_store().upsert(rec)
        with patch('backend.inference.supervisor.TorchPeftEngine', return_value=fixtures.FakeEngine('test')) as engine:
            self.sup.activate_adapter(rec.id)
            engine.assert_called_once_with(str(path), str(self.root))
        path.rmdir()
        with self.assertRaises(ValueError): _resolve_base(record.id)

    async def test_raw_and_path_documents_match_and_bad_pdf_is_400(self):
        with patch.object(rag.embeddings, 'is_available', return_value=True), patch.object(rag.embeddings, 'embed_texts', side_effect=lambda texts, model: np.ones((len(texts),2),dtype=np.float32)):
            for kind, content in [('html','<script>irrelevant</script><p>Actual answer</p>'), ('md','# Actual answer\n[link](https://example.com)')]:
                path = self.root/('doc.'+kind)
                path.write_text(content)
                self.assertEqual(chunk_text(extract_content(content, kind)), chunk_text(extract_text(str(path), kind)))
                for body in ({'name':path.name, 'kind':kind, 'content':content}, {'path':str(path)}):
                    response = await self.client.post('/rag/documents', json=body)
                    self.assertEqual(response.status_code, 201, response.text)
            path = self.root/'broken.pdf'
            path.write_bytes(b'%PDF-1.7\ntruncated')
            response = await self.client.post('/rag/documents', json={'path':str(path)})
            self.assertEqual(response.status_code, 400, response.text)
            response = await self.client.post('/rag/documents', json={'name':'raw.pdf','content':'%PDF-1.7'})
            self.assertEqual(response.status_code, 400)


class DatasetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = DatasetStore(self.temp.name, str(self.root/'data.db'))

    def test_bom_normalization_raw_and_file(self):
        for fmt, content in [('csv','instruction,output\nQuestion,Answer\n'), ('jsonl','{"text":"valid"}')]:
            expected = Path(self.store.ingest_content('x', content, fmt).path).read_text()
            self.assertEqual(Path(self.store.ingest_content('x', '\ufeff'+content, fmt).path).read_text(), expected)
            path = self.root/('bom.'+fmt)
            path.write_text('\ufeff'+content)
            self.assertEqual(Path(self.store.ingest_path(str(path)).path).read_text(), expected)

    def test_invalid_values_and_csv_size_boundaries(self):
        for value in (None, True, 123, [], {}, ''):
            for key in ('input','instruction','content'):
                with self.assertRaisesRegex(ValueError, 'Row 1'):
                    self.store.ingest_content('x', json.dumps({key:value}), 'jsonl')
        for content in ('label,text\ncategory\n', 'label,text\ncategory,\n', 'prompt,completion\nhello\n'):
            with self.assertRaisesRegex(ValueError, 'Row 2'):
                self.store.ingest_content('x', content, 'csv')
        for length in (CSV_FIELD_LIMIT-1, CSV_FIELD_LIMIT):
            self.assertEqual(self.store.ingest_content('x','text\n'+'x'*length,'csv').rows,1)
        with self.assertRaisesRegex(ValueError, 'maximum field size'):
            self.store.ingest_content('x','text\n'+'x'*(CSV_FIELD_LIMIT+1),'csv')

    def test_running_and_queued_dataset_ownership(self):
        entered, release = threading.Event(), threading.Event()
        def train(job, cfg, path, *args):
            job.status = 'running'
            entered.set()
            release.wait(5)
            self.assertTrue(Path(path).is_file())
            job.status = 'done'
        from backend.adapters.store import AdapterStore
        with patch('backend.training.jobs.run_training', train):
            manager = TrainingJobManager(self.temp.name, AdapterStore(str(self.root/'adapters.db')), self.store)
            try:
                datasets = [self.store.ingest_content('x','sample') for _ in range(2)]
                manager.start(TrainingConfig(base_model='test',dataset_id=datasets[0].id))
                self.assertTrue(entered.wait(5))
                manager.start(TrainingConfig(base_model='test',dataset_id=datasets[1].id))
                for dataset in datasets:
                    with self.assertRaisesRegex(RuntimeError, 'in use'): self.store.delete(dataset.id)
            finally:
                release.set()
                manager.shutdown()
            for dataset in datasets: self.assertTrue(self.store.delete(dataset.id))

    def test_start_delete_race(self):
        from backend.adapters.store import AdapterStore
        with patch('backend.training.jobs.run_training', lambda job, *args: setattr(job,'status','done')):
            manager = TrainingJobManager(self.temp.name, AdapterStore(str(self.root/'adapters.db')), self.store)
            try:
                for _ in range(20):
                    dataset = self.store.ingest_content('x','sample')
                    barrier = threading.Barrier(2)
                    def start():
                        barrier.wait()
                        try: return manager.start(TrainingConfig(base_model='test',dataset_id=dataset.id))
                        except ValueError: return None
                    def delete():
                        barrier.wait()
                        try: return self.store.delete(dataset.id)
                        except RuntimeError: return False
                    with ThreadPoolExecutor(2) as pool:
                        a, b = pool.submit(start), pool.submit(delete)
                        job, deleted = a.result(), b.result()
                    if job is not None and not deleted: self.assertTrue(Path(dataset.path).exists())
            finally: manager.shutdown()


class DisconnectTests(unittest.IsolatedAsyncioTestCase):
    async def test_disconnect_after_first_token_closes_and_releases(self):
        from backend.routes.inference import _response
        sup = fixtures.supervisor()
        closed = asyncio.Event()
        class Engine(fixtures.FakeEngine):
            async def complete_stream(self, *args, **kwargs):
                try:
                    yield 'first'
                    await asyncio.sleep(10)
                    yield 'unreachable'
                finally: closed.set()
        sup._engine = Engine('test')
        started = time.monotonic()
        class Request:
            async def is_disconnected(self): return time.monotonic()-started > .05
        result = await asyncio.wait_for(_response(sup.complete_stream('hi'), False, 'test', request=Request()), 2)
        self.assertEqual(result.status_code,499)
        self.assertTrue(closed.is_set())
        sup.unload()


class PdfTests(unittest.TestCase):
    def test_empty_encrypted_and_image_only_documents(self):
        try:
            from pypdf import PdfWriter
        except ImportError:
            self.skipTest('pypdf optional dependency unavailable')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'document.pdf'
            for mode in ('empty', 'blank', 'encrypted'):
                writer = PdfWriter()
                if mode != 'empty': writer.add_blank_page(width=100, height=100)
                if mode == 'encrypted': writer.encrypt('password')
                writer.write(path)
                with self.assertRaises(ValueError): extract_text(str(path), 'pdf')


class HttpDisconnectTests(unittest.TestCase):
    def test_dropped_nonstreaming_http_connection_releases_generation(self):
        import socket
        import uvicorn
        from fastapi import FastAPI
        from backend.routes import inference
        entered, closed = threading.Event(), threading.Event()
        class Engine(fixtures.FakeEngine):
            async def complete_stream(self, *args, **kwargs):
                try:
                    entered.set()
                    yield 'first'
                    await asyncio.sleep(10)
                    yield 'unreachable'
                finally: closed.set()
        sup = fixtures.supervisor()
        sup._engine = Engine('test')
        sup.model_ref = 'test'
        app = FastAPI()
        app.include_router(inference.router)
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        port = listener.getsockname()[1]
        server = uvicorn.Server(uvicorn.Config(app, log_level='error', lifespan='off'))
        worker = threading.Thread(target=server.run, kwargs={'sockets':[listener]}, daemon=True)
        with patch.object(inference, 'get_supervisor', return_value=sup):
            worker.start()
            try:
                deadline = time.monotonic()+5
                while not server.started and time.monotonic()<deadline: time.sleep(.01)
                self.assertTrue(server.started)
                body = json.dumps({'prompt':'hello', 'stream':False}).encode()
                with socket.create_connection(('127.0.0.1',port),timeout=5) as client:
                    client.sendall(b'POST /inference/completion HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)
                    self.assertTrue(entered.wait(5))
                self.assertTrue(closed.wait(3), 'generation continued after the HTTP client disconnected')
                sup.unload()
            finally:
                server.should_exit = True
                worker.join(5)
                listener.close()
            self.assertFalse(worker.is_alive())
