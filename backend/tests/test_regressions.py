"""Offline regressions for the September audit; run with unittest discovery."""
import asyncio
from contextlib import nullcontext
import json
import os
from pathlib import Path
import queue
import sqlite3
import tempfile
import threading
import time
import types
import unittest
from unittest.mock import patch, MagicMock
import zipfile

import httpx
import numpy as np

from backend.server import app
from backend.routes import adapters, rag, inference, models
from backend.adapters.store import AdapterStore
from backend.inference.engines.base import BaseEngine
from backend.inference.engines.llamacpp import LlamaCppEngine
from backend.inference.engines.torch import TorchPeftEngine
from backend.inference.engines.vllm import VllmEngine
from backend.inference.supervisor import InferenceSupervisor
from backend.inference.schemas import ChatMessage, ChatRequest
from backend.models.registry import ModelRegistry
from backend.models.schemas import ModelRecord
from backend.models.store import ModelStore
from backend.rag.chunking import chunk_text, _strip_markdown
from backend.training.datasets import DatasetStore, _normalize_jsonl, _normalize_csv
from backend.training.jobs import TrainingJobManager
from backend.training.schemas import TrainingConfig
from backend.request_work import request_work, ClientDisconnected


class FakeEngine(BaseEngine):
    name = "torch"
    def load(self): self._loaded = True
    async def chat_stream(self, messages, **kwargs):
        yield "hello "
        yield "world"
    async def complete_stream(self, prompt, **kwargs):
        yield "completion"
    def count_tokens(self, text): return len(text)
    def count_messages(self, messages): return sum(len(m.content) + 8 for m in messages)


def supervisor():
    with patch('backend.inference.supervisor.detect_gpu', return_value=(False, 'test')), \
         patch.object(LlamaCppEngine, 'is_available', return_value=True), \
         patch.object(VllmEngine, 'is_available', return_value=False), \
         patch.object(TorchPeftEngine, 'is_available', return_value=True):
        return InferenceSupervisor()


class ApiTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.env = patch.dict(os.environ, {'MAGNITE_HOME': self.temp.name})
        self.env.start()
        adapters._adapter_setup()
        rag._rag_setup()
        models._registry_setup()
        self.sup = supervisor()
        self.sup.set_adapter_store(adapters._get_store())
        self.sup._engine = FakeEngine('test')
        self.sup.model_ref = 'test'
        self.sup_patch = patch.object(inference, 'get_supervisor', return_value=self.sup)
        self.sup_patch.start()
        self.adapter_sup_patch = patch.object(adapters, 'get_supervisor', return_value=self.sup)
        self.adapter_sup_patch.start()
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app, raise_app_exceptions=False), base_url='http://test')

    async def asyncTearDown(self):
        await self.client.aclose()
        self.sup_patch.stop()
        self.adapter_sup_patch.stop()
        self.env.stop()
        self.temp.cleanup()

    def archive(self, entries=None, record=None):
        path = self.root / 'adapter.zip'
        rec = dict(id='adapter:test', name='test', base_model='test/base', path='ignored')
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr('record.json', json.dumps(rec if record is None else record))
            z.writestr('weights/adapter_config.json', json.dumps(dict(peft_type='LORA', r=2, lora_alpha=4, target_modules=['q_proj'])))
            from safetensors.numpy import save
            z.writestr('weights/adapter_model.safetensors', save({'lora_A.weight': np.zeros((2, 2), dtype=np.float32)}))
            for name, value in (entries or {}).items():
                z.writestr(name, value)
        return path

    async def test_archive_traversal_rejected_without_writes(self):
        for name in ('weights/../escaped', 'weights//tmp/escaped', 'weights/../../escaped', 'weights/a\\b'):
            # Double slash is normalized inside the destination; absolute remainder must be rejected.
            path = self.archive({name: 'marker'})
            response = await self.client.post('/adapters/import', json={'path': str(path)})
            self.assertEqual(response.status_code, 400, (name, response.text))
        self.assertFalse((self.root/'adapters/imports/escaped').exists())

    async def test_valid_archive_roundtrip_and_duplicates(self):
        response = await self.client.post('/adapters/import', json={'path': str(self.archive())})
        self.assertEqual(response.status_code, 201, response.text)
        rec = response.json()
        self.assertTrue(Path(rec['path']).is_relative_to(self.root/'adapters/imports'))
        response = await self.client.post('/adapters/import', json={'path': str(self.root/'adapter.zip')})
        self.assertEqual(response.status_code, 409)
        response = await self.client.post('/adapters/export/adapter:test')
        self.assertEqual(response.status_code, 200)
        exported = self.root/'adapters/exports'/Path(response.json()['download_path']).name
        self.assertTrue(exported.exists())
        self.assertEqual((await self.client.delete('/adapters/adapter:test')).status_code, 204)
        self.assertFalse(Path(rec['path']).exists())
        self.assertEqual((await self.client.post('/adapters/import', json={'path': str(exported)})).status_code, 201)

    async def test_invalid_archive_metadata_and_missing_weights(self):
        for record in ({}, dict(id='..',name='bad',base_model='test',path='')):
            response = await self.client.post('/adapters/import', json={'path': str(self.archive(record=record))})
            self.assertEqual(response.status_code, 400, response.text)
        with zipfile.ZipFile(self.root/'empty.zip', 'w') as z:
            z.writestr('record.json', json.dumps(dict(id='empty',name='empty',base_model='test',path='')))
        self.assertEqual((await self.client.post('/adapters/import', json={'path': str(self.root/'empty.zip')})).status_code, 400)

    async def test_load_validation_and_engine_validation(self):
        for body in ({'model':123}, {'model':'x','engine':'bogus'}):
            response = await self.client.post('/inference/load', json=body)
            self.assertEqual(response.status_code, 422, response.text)
        self.assertIsInstance(self.sup.make_engine('torch', 'test'), TorchPeftEngine)
        with self.assertRaises(ValueError): self.sup.make_engine('bogus','test')

    async def test_nonstreaming_chat_and_completion(self):
        response = await self.client.post('/inference/chat', json={'messages':[{'role':'user','content':'hi'}],'stream':False})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()['choices'][0]['message']['content'], 'hello world')
        response = await self.client.post('/inference/completion', json={'prompt':'hi','stream':False})
        self.assertEqual(response.json()['choices'][0]['text'], 'completion')

    async def test_streaming_and_errors(self):
        response = await self.client.post('/inference/chat', json={'messages':[{'role':'user','content':'hi'}]})
        self.assertIn('"done": true', response.text)
        async def broken(*args, **kwargs):
            yield 'partial'
            raise RuntimeError('generation failed')
        with patch.object(self.sup._engine, 'chat_stream', broken):
            response = await self.client.post('/inference/chat', json={'messages':[{'role':'user','content':'hi'}]})
            self.assertIn('"error": "generation failed"',response.text)
            self.assertNotIn('"done": true',response.text)
        self.sup._engine = None
        response = await self.client.post('/inference/chat', json={'messages':[]})
        self.assertEqual(response.status_code, 400)
        self.assertIn('Load a model', response.text)

    async def test_rag_configuration_validation_and_merge(self):
        response = await self.client.post('/rag/config', json={'chunk_size':64,'chunk_overlap':64})
        self.assertEqual(response.status_code, 422)
        await self.client.post('/rag/config', json={'min_score':.8,'model':'test/model'})
        response = await self.client.post('/rag/config', json={'top_k':5})
        self.assertEqual(response.json()['min_score'], .8)
        self.assertEqual(response.json()['model'], 'test/model')
        response = await self.client.post('/rag/documents', json={'name':'bad','content':'hello','chunk_size':-1})
        self.assertEqual(response.status_code, 422)

    async def test_ingest_query_delete_and_model_change_guard(self):
        with patch.object(rag.embeddings,'is_available',return_value=True), \
             patch.object(rag.embeddings,'embed_texts',side_effect=lambda texts,model: np.array([[1.,0.]]*len(texts), dtype=np.float32)):
            response = await self.client.post('/rag/documents', json={'name':'facts.txt','content':'Paris has the Eiffel Tower.'})
            self.assertEqual(response.status_code, 201, response.text)
            doc = response.json()['id']
            response = await self.client.post('/rag/query', json={'query':'Paris'})
            self.assertEqual(response.json()['results'][0]['document_id'],doc)
            self.assertEqual((await self.client.post('/rag/config', json={'model':'different'})).status_code,409)
            self.assertEqual((await self.client.delete('/rag/documents/'+doc)).status_code,204)
            self.assertEqual((await self.client.get('/rag/documents')).json(),[])

    async def test_ingest_does_not_register_cancelled_or_stale_embeddings(self):
        def embed_and_change_model(texts, model):
            rag._get_store().update_config({'model':'changed-during-ingestion'})
            return np.ones((len(texts),2),dtype=np.float32)
        with patch.object(rag.embeddings,'is_available',return_value=True), \
             patch.object(rag.embeddings,'embed_texts',side_effect=embed_and_change_model):
            response = await self.client.post('/rag/documents',json={'name':'facts.txt','content':'Some facts'})
            self.assertEqual(response.status_code,409,response.text)
            self.assertEqual(rag._get_store().count_chunks(),0)
        cancelled=threading.Event()
        cancelled.set()
        with patch.object(rag.embeddings,'is_available',return_value=True), \
             patch.object(rag.embeddings,'embed_texts',return_value=np.ones((1,2),dtype=np.float32)):
            with self.assertRaises(Exception) as error:
                rag._ingest_document(rag.RagIngestRequest(name='facts.txt',content='Some facts'),cancelled)
            self.assertIn('cancelled',str(error.exception.detail))
            self.assertEqual(rag._get_store().count_chunks(),0)

    async def test_rag_context_budget_and_failure(self):
        req = ChatRequest(messages=[ChatMessage(role='user',content='question')],max_tokens=256,rag={'enabled':True})
        engine = FakeEngine('test')
        with patch.object(inference,'injectable_context',return_value='context '*2000):
            messages = inference._with_rag_context(req,engine)
            self.assertEqual(messages[0].role,'system')
            self.assertLessEqual(engine.count_messages(messages)+req.max_tokens,engine.context_size())
        response = await self.client.post('/inference/chat',json=req.model_dump())
        self.assertEqual(response.status_code,400)
        self.assertIn('RAG',response.text) if 'unavailable' in response.text else self.assertIn('documents',response.text)

    async def test_registered_model_resolution(self):
        model_dir = self.root/'model'
        model_dir.mkdir()
        (model_dir/'model.gguf').write_bytes(b'gguf')
        record = ModelRecord(id='huggingface:owner--model',name='model',ref='owner/model',source='huggingface',path=str(model_dir))
        models._get_registry()._store.upsert(record)
        self.assertEqual(self.sup._resolve_engine(None,record.id),('llamacpp',str(model_dir)))
        self.assertEqual(LlamaCppEngine(str(model_dir))._resolve_gguf_path(),str(model_dir/'model.gguf'))
        with patch.object(self.sup,'make_engine',return_value=FakeEngine('test')) as factory:
            response=await self.client.post('/inference/load',json={'model':record.id})
            self.assertEqual(response.status_code,200,response.text)
            factory.assert_called_once_with('llamacpp',str(model_dir))
        (model_dir/'model.gguf').unlink()
        self.assertEqual(self.sup._resolve_engine(None,record.id)[0],'torch')

    async def test_lifecycle_rejects_concurrent_changes(self):
        stream=self.sup.chat_stream([ChatMessage(role='user',content='hi')])
        await anext(stream)
        with self.assertRaisesRegex(RuntimeError,'busy'): self.sup.unload()
        with self.assertRaisesRegex(RuntimeError,'busy'): self.sup.load_model('other')
        await stream.aclose()
        self.sup.unload()
        self.assertIsNone(self.sup._engine)

    async def test_runtime_settings(self):
        response=await self.client.post('/inference/settings',json={'engine_override':2})
        self.assertEqual(response.status_code,200)
        self.assertEqual(self.sup._resolve_engine(None,'repo/model')[0],'llamacpp')
        self.assertEqual((await self.client.post('/inference/settings',json={'engine_override':3})).status_code,422)


class StreamingTests(unittest.IsolatedAsyncioTestCase):
    async def test_disconnected_model_load_discards_and_unloads_result(self):
        sup = supervisor()
        entered = threading.Event()
        class SlowEngine(FakeEngine):
            def load(self):
                entered.set()
                while not self.cancel_load.is_set():
                    time.sleep(.005)
                self._loaded = True
        class Request:
            async def is_disconnected(self): return entered.is_set()
        engine = SlowEngine('test')
        with patch.object(sup, 'make_engine', return_value=engine):
            with self.assertRaises(ClientDisconnected):
                await asyncio.wait_for(request_work(Request(), lambda cancelled:
                    sup.load_model('test', 'torch', cancelled)), 5)
        self.assertFalse(engine.loaded)
        self.assertIsNone(sup._engine)
        sup.unload()  # Lifecycle lock must also have been released.

    async def test_cancelled_preparation_keeps_model_reserved_until_worker_exits(self):
        sup = supervisor()
        sup._engine = FakeEngine('test')
        entered = threading.Event()
        release = threading.Event()
        def prepare(engine):
            entered.set()
            release.wait(5)
            return [ChatMessage(role='user', content='hello')]
        stream = sup.chat_stream([], prepare=prepare)
        task = asyncio.create_task(anext(stream))
        self.assertTrue(await asyncio.to_thread(entered.wait, 5))
        task.cancel()
        await asyncio.sleep(.01)
        try:
            with self.assertRaisesRegex(RuntimeError, 'busy'):
                sup.unload()
        finally:
            release.set()
        with self.assertRaises(asyncio.CancelledError):
            await task
        sup.unload()

    async def test_disconnect_before_first_token_closes_generation(self):
        closed = asyncio.Event()
        async def generate():
            try:
                await asyncio.Event().wait()
                yield 'unreachable'
            finally:
                closed.set()
        class Request:
            async def is_disconnected(self): return True
        response = await inference._response(generate(), True, 'test', request=Request())
        self.assertEqual(response.status_code, 499)
        self.assertTrue(closed.is_set())

    async def test_asgi_send_failure_releases_engine_before_headers(self):
        sup = supervisor()
        sup._engine = FakeEngine('test')
        source = sup.chat_stream([ChatMessage(role='user',content='hello')])
        response = await inference._response(source, True, 'test')
        async def receive(): return {'type':'http.disconnect'}
        async def send(message): raise OSError('client closed')
        with self.assertRaises(Exception):
            await response({'type':'http','asgi':{'spec_version':'2.4'}}, receive, send)
        sup.unload()  # Must not still be marked busy.
        self.assertIsNone(sup._engine)

    async def test_torch_first_token_precedes_completion_and_close_cancels(self):
        completed=threading.Event()
        class Streamer:
            def __init__(self,*args,**kwargs): self.q=queue.Queue()
            def end(self): self.q.put(None)
            def __iter__(self): return self
            def __next__(self):
                value=self.q.get(timeout=5)
                if value is None: raise StopIteration
                return value
        class Inputs(dict):
            def to(self,device): return self
        class Tokenizer:
            def __call__(self,*args,**kwargs): return Inputs()
        class Model:
            device='cpu'
            def generate(self,**kwargs):
                kwargs['streamer'].q.put('first token')
                cancel=kwargs['stopping_criteria'][0]
                while not cancel(None,None): time.sleep(.005)
                completed.set()
        torch=types.SimpleNamespace(inference_mode=nullcontext)
        transformers=types.SimpleNamespace(TextIteratorStreamer=Streamer,StoppingCriteria=object,StoppingCriteriaList=list)
        engine=TorchPeftEngine('test')
        engine._model=Model()
        engine._tokenizer=Tokenizer()
        with patch.dict('sys.modules',{'torch':torch,'transformers':transformers}):
            stream=engine.complete_stream('hello')
            self.assertEqual(await asyncio.wait_for(anext(stream),5),'first token')
            self.assertFalse(completed.is_set())
            await asyncio.wait_for(stream.aclose(),5)
            self.assertTrue(completed.is_set())

    async def test_vllm_http_errors_propagate(self):
        real_client=httpx.AsyncClient
        transport=httpx.MockTransport(lambda req: httpx.Response(503,json={'error':'unavailable'}))
        with patch('backend.inference.engines.vllm.httpx.AsyncClient',side_effect=lambda **kwargs: real_client(transport=transport)):
            engine=VllmEngine('test',port=12345)
            with self.assertRaises(httpx.HTTPStatusError):
                _=[text async for text in engine.complete_stream('hi')]


class StoreTests(unittest.TestCase):
    def test_instruction_and_bad_rows(self):
        sample={'instruction':'Say hello','input':'to Alice','output':'Hello Alice'}
        result=_normalize_jsonl(json.dumps(sample))[0]['messages']
        self.assertEqual(result[0]['content'],'Say hello\nto Alice')
        self.assertEqual(result[1]['content'],'Hello Alice')
        csv_result = _normalize_csv('instruction,input,output\nSay hello,to Alice,Hello Alice\n')
        self.assertEqual(csv_result[0]['messages'], result)
        for data in ('{"text":"valid"}\ninvalid', '{}', '{"messages":[]}', '{"text":null}'):
            with self.assertRaises(ValueError): _normalize_jsonl(data)

    def test_chunk_validation_and_code_preservation(self):
        for size,overlap in ((-1,0),(0,0),(64,64),(64,-1)):
            with self.assertRaises(ValueError): chunk_text('sample',size,overlap)
        text=_strip_markdown('# Usage\n```python\nprint("hello")\n```\nUse `some_function`.')
        self.assertIn('print("hello")',text)
        self.assertIn('some_function',text)

    def test_ollama_delete_calls_server(self):
        with tempfile.TemporaryDirectory() as directory:
            registry=ModelRegistry(ModelStore(str(Path(directory)/'models.db')))
            rec=ModelRecord(id='ollama:toy',name='toy',source='ollama',ref='toy',path='toy')
            registry._store.upsert(rec)
            response=httpx.Response(200,request=httpx.Request('DELETE','http://ollama'))
            with patch('backend.models.registry.httpx.request',return_value=response) as request:
                self.assertTrue(registry.delete_model(rec.id))
                self.assertEqual(request.call_args.kwargs['json'],{'model':'toy'})
            self.assertIsNone(registry._store.get(rec.id))

    def test_hf_cache_deletion_preserves_shared_blobs(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            repo=root/'models--owner--model'
            blobs=repo/'blobs'
            blobs.mkdir(parents=True)
            (blobs/'shared').write_bytes(b'shared')
            (blobs/'unique').write_bytes(b'unique')
            for revision in ('a'*40,'b'*40):
                snapshot=repo/'snapshots'/revision
                snapshot.mkdir(parents=True)
                (snapshot/'shared').symlink_to(blobs/'shared')
            first=repo/'snapshots'/('a'*40)
            (first/'unique').symlink_to(blobs/'unique')
            registry=ModelRegistry(ModelStore(str(root/'models.db')))
            rec=ModelRecord(id='hf',name='hf',source='huggingface',ref='owner/model',path=str(first))
            registry._remove_hf(rec)
            self.assertFalse(first.exists())
            self.assertFalse((blobs/'unique').exists())
            self.assertTrue((blobs/'shared').exists())

    def test_training_queue_cancel_and_history(self):
        with tempfile.TemporaryDirectory() as directory:
            store=DatasetStore(directory,str(Path(directory)/'datasets.db'))
            data=store.ingest_content('test','hello')
            adapters_store=AdapterStore(str(Path(directory)/'adapters.db'))
            entered=threading.Event()
            release=threading.Event()
            calls=[]
            def train(job,cfg,path,adapter_dir,adapter_store,event):
                calls.append(job.id)
                job.status='running'
                entered.set()
                release.wait(5)
                job.status='done'
            with patch('backend.training.jobs.run_training',side_effect=train):
                manager=TrainingJobManager(directory,adapters_store,store)
                cfg=TrainingConfig(base_model='test',dataset_id=data.id)
                first=manager.start(cfg)
                self.assertTrue(entered.wait(5))
                second=manager.start(cfg)
                self.assertEqual(manager.get(second.id).status,'queued')
                self.assertTrue(manager.cancel(second.id))
                release.set()
                manager.shutdown()
                self.assertEqual(calls,[first.id])
            restored=TrainingJobManager(directory,adapters_store,store)
            self.assertEqual(restored.get(first.id).status,'done')
            self.assertEqual(restored.get(second.id).status,'cancelled')
            restored.shutdown()


if __name__ == '__main__': unittest.main()
