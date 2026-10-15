"""Reproduce October audit findings; no downloads and only temporary writes.
Run: PYTHONPATH=backend/src .venv/bin/python docs/testing/extreme_bug_probe.py
Observations are evidence, not assertions that buggy behavior is desirable.
"""
import asyncio
import csv
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import types
from unittest.mock import patch, MagicMock
import zipfile

import httpx
import numpy as np

from backend.server import app
from backend.routes import training, adapters, rag, inference
from backend.inference.engines.base import BaseEngine
from backend.inference.supervisor import InferenceSupervisor
from backend.training.trainer import _resolve_base
from backend.models.schemas import ModelRecord
from backend.routes import models
from backend.training.schemas import TrainingConfig, TrainingJob
from backend.training.trainer import run_training


def emit(case, **values):
    print(json.dumps({'case': case, **values}, ensure_ascii=True), flush=True)


class Engine(BaseEngine):
    name = 'torch'
    def load(self): self._loaded = True
    async def chat_stream(self, messages, **kwargs): yield 'from loaded model'
    async def complete_stream(self, prompt, **kwargs): yield 'from loaded model'


async def main(root):
    os.environ['MAGNITE_HOME'] = str(root)
    training._training_setup()
    adapters._adapter_setup()
    rag._rag_setup()
    models._registry_setup()
    with patch.object(InferenceSupervisor, '__init__', lambda self: None):
        sup = InferenceSupervisor()
    sup._operation = threading.Lock()
    sup._engine = Engine('loaded-A')
    sup.model_ref = 'loaded-A'
    sup.active_adapter = None
    async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app, raise_app_exceptions=False), base_url='http://audit') as c:
        for case, fmt, content in [
            ('CSV_LONG_FIELD', 'csv', 'text\n' + 'x' * (csv.field_size_limit()+1)),
            ('CSV_MISSING_TEXT', 'csv', 'label,text\ncategory\n'),
            ('JSON_NULL_CONTENT', 'jsonl', '{"content":null}'),
            ('JSON_OBJECT_CONTENT', 'jsonl', '{"content":{"bad":"shape"}}'),
            ('JSON_BOM', 'jsonl', '\ufeff{"text":"valid UTF-8 BOM file"}'),
            ('CSV_BOM', 'csv', '\ufeffinstruction,output\nQuestion,Answer\n'),
            ('UNICODE_SURROGATE', 'text', '\ud800'),
        ]:
            result = await c.post('/datasets/ingest', content=json.dumps({'name':case,'format':fmt,'content':content}), headers={'content-type':'application/json'})
            stored = Path(result.json()['path']).read_text() if result.status_code == 201 else None
            emit(case, status=result.status_code, body=result.text[:300], stored=stored)

        entered, release = threading.Event(), threading.Event()
        def blocked(job, cfg, path, *args):
            job.status='running'
            entered.set()
            release.wait(10)
            try:
                Path(path).read_text()
                job.status='done'
            except OSError as exc:
                job.status='error'
                job.error=str(exc)
        with patch('backend.training.jobs.run_training', blocked):
            ds = (await c.post('/datasets/ingest', json={'name':'busy','content':'sample'})).json()
            job = (await c.post('/training/start', json={'base_model':'test/base','dataset_id':ds['id']})).json()
            await asyncio.to_thread(entered.wait, 2)
            result = await c.delete('/datasets/'+ds['id'])
            release.set()
            for _ in range(100):
                current=training._manager.get(job['id'])
                if current.status in ('done','error','cancelled'): break
                await asyncio.sleep(.01)
            emit('DELETE_BUSY_DATASET', status=result.status_code, job_status=current.status, error=current.error)

        def archive(identifier, marker, valid=False):
            dest=root/(identifier.replace(':','_')+'.zip')
            with zipfile.ZipFile(dest,'w') as z:
                z.writestr('record.json',json.dumps(dict(id=identifier,name=marker,base_model='test/base',path='ignored')))
                z.writestr('weights/adapter_config.json', json.dumps(dict(peft_type='LORA', r=2, lora_alpha=4, target_modules=['q_proj'])))
                from safetensors.numpy import save
                weights = save({'lora_A.weight': np.zeros((2, 2), dtype=np.float32)}) if valid else 'not a tensor file '+marker
                z.writestr('weights/adapter_model.safetensors', weights)
            return dest
        for identifier, marker in [('adapter:collision','FIRST'),('adapter-collision','SECOND')]:
            result=await c.post('/adapters/import',json={'path':str(archive(identifier,marker))})
            emit('CORRUPT_WEIGHTS_IMPORT', id=identifier, status=result.status_code, registered_status=result.json().get('status'))
            if result.status_code == 400:
                result = await c.post('/adapters/import', json={'path': str(archive(identifier, marker, valid=True))})
                assert result.status_code == 201, result.text
        one=(await c.post('/adapters/export/adapter:collision')).json()
        before=await c.get(one['download_path'])
        two=(await c.post('/adapters/export/adapter-collision')).json()
        after=await c.get(one['download_path'])
        emit('EXPORT_COLLISION', first_path=one['download_path'], second_path=two['download_path'], first_download_changed=before.content!=after.content)
        # Missing source weights should not silently yield a successful backup.
        rec=adapters._get_store().get('adapter:collision')
        rec.path=str(root/'missing-weights')
        adapters._get_store().upsert(rec)
        result=await c.post('/adapters/export/'+rec.id)
        emit('EXPORT_MISSING_WEIGHTS',status=result.status_code)

        def embed(texts, model): return np.tile(np.array([[1.,0.]],dtype=np.float32),(len(texts),1))
        with patch.object(rag.embeddings,'is_available',return_value=True), patch.object(rag.embeddings,'embed_texts',side_effect=embed):
            html='<script>secret irrelevant javascript</script><p>Actual answer</p>'
            path=root/'example.html'
            path.write_text(html)
            raw=await c.post('/rag/documents',json={'name':'raw.html','content':html,'kind':'html'})
            file=await c.post('/rag/documents',json={'path':str(path)})
            query=(await c.post('/rag/query',json={'query':'answer'})).json()
            emit('RAW_HTML',raw_status=raw.status_code,file_status=file.status_code,texts=[x['text'] for x in query['results']])
            bad=root/'broken.pdf'
            bad.write_bytes(b'%PDF-1.7\nthis is truncated')
            result=await c.post('/rag/documents',json={'path':str(bad)})
            emit('BROKEN_PDF',status=result.status_code,body=result.text[:300])

        with patch.object(inference,'get_supervisor',return_value=sup):
            result=await c.post('/inference/chat',json={'model':'different-B','messages':[{'role':'user','content':'hello'}],'stream':False})
            emit('MODEL_IGNORED',status=result.status_code,body=result.json())
            result=await c.post('/inference/chat',json={'messages':[],'stream':False})
            emit('EMPTY_MESSAGES',status=result.status_code)

        local=root/'external-snapshot'
        local.mkdir()
        models._get_registry()._store.upsert(ModelRecord(id='huggingface:owner--model',name='model',source='huggingface',ref='owner/model',path=str(local)))
        emit('TRAINING_RESOLUTION',registered_path=str(local),resolved_training_base=_resolve_base('huggingface:owner--model'))

        class Request:
            checks=0
            disconnected_at=time.monotonic()+.05
            async def is_disconnected(self):
                self.checks+=1
                return time.monotonic() >= self.disconnected_at
        request=Request()
        async def gen():
            yield 'first'
            await asyncio.sleep(.25)
            yield 'after disconnect'
        response=await inference._response(gen(),False,'test',request=request)
        emit('NONSTREAM_DISCONNECT_WATCH',disconnect_checks=request.checks,disconnected=time.monotonic() >= request.disconnected_at,status=getattr(response, 'status_code', 200), body=response['choices'][0] if isinstance(response, dict) else None)

        # Exercise real trainer cancellation at the save boundary with fake ML.
        cancel=threading.Event()
        model=MagicMock()
        model.config.architectures=['TestModel']
        def save(path):
            cancel.set()
            Path(path,'adapter_model.safetensors').write_text('fixture')
        model.save_pretrained.side_effect=save
        trainer=MagicMock()
        trainer.train.return_value=types.SimpleNamespace(global_step=1,training_loss=1.)
        ml={name:MagicMock() for name in ('torch','AutoModelForCausalLM','AutoTokenizer','BitsAndBytesConfig','DataCollatorForLanguageModeling','TrainingArguments','Trainer')}
        ml['TrainerCallback']=object
        ml['torch'].cuda.is_available.return_value=False
        ml['AutoModelForCausalLM'].from_pretrained.return_value=model
        ml['Trainer'].return_value=trainer
        peft=types.ModuleType('peft')
        peft.LoraConfig=MagicMock()
        peft.get_peft_model=MagicMock(return_value=model)
        peft.prepare_model_for_kbit_training=MagicMock(return_value=model)
        datasets=types.ModuleType('datasets')
        datasets.Dataset=MagicMock()
        cfg=TrainingConfig(base_model='test/base',dataset_id='fixture',max_steps=1,target_modules='all-linear')
        job=TrainingJob(id='late-cancel',name='late cancel',status='queued',base_model='test/base',dataset_id='fixture')
        with patch('backend.training.trainer._require_ml',return_value=ml), patch('backend.training.trainer._tokenize_jsonl',return_value=[{'input_ids':[1]}]), patch.dict('sys.modules',{'peft':peft,'datasets':datasets}):
            run_training(job,cfg,'unused',str(root/'adapters'),adapters._get_store(),cancel)
        emit('CANCEL_DURING_SAVE',cancelled=cancel.is_set(),status=job.status,adapter_registered=bool(adapters._get_store().get(job.adapter_id or '')))

        # Exercise type/range validation without allowing accepted jobs/downloads.
        matrix={
            '/inference/settings':('engine_override',0),
            '/inference/chat':('max_tokens',{'messages':[{'role':'user','content':'test'}]}),
            '/inference/completion':('temperature',{'prompt':'hello'}),
            '/rag/config':('chunk_size',{}),
            '/rag/query':('top_k',{'query':'test'}),
            '/datasets/ingest':('content',{'name':'fuzz','format':'text'}),
            '/adapters/import':('path',{}),
            '/training/start':('rank',{'base_model':'test/base','dataset_id':'missing'}),
        }
        values=[None,True,False,0,-1,1,2,8193,10**30,0.5,'',' ','x',[],{},['x']]
        codes={}
        failures=[]
        for route,(field,base) in matrix.items():
            for value in values:
                body=dict(base) if isinstance(base,dict) else {}
                body[field]=value
                with patch.object(inference,'get_supervisor',return_value=sup), patch.object(rag.embeddings,'is_available',return_value=False):
                    result=await c.post(route,json=body)
                codes[str(result.status_code)]=codes.get(str(result.status_code),0)+1
                if result.status_code>=500 and result.status_code!=503:
                    failures.append({'route':route,'value':value,'status':result.status_code})
        emit('BOUNDARY_MATRIX',requests=sum(codes.values()),statuses=codes,unexpected_server_errors=failures)

        # Bounded concurrency test: 40 writes, 40 reads, then 40 deletions.
        results=await asyncio.gather(*[c.post('/datasets/ingest',json={'name':f'parallel-{i}','content':'sample'}) for i in range(40)])
        ids=[r.json()['id'] for r in results if r.status_code==201]
        reads=await asyncio.gather(*[c.get('/datasets/'+i) for i in ids])
        deletes=await asyncio.gather(*[c.delete('/datasets/'+i) for i in ids])
        emit('CONCURRENT_DATASET_CRUD',created=len(ids),unique=len(set(ids)),read_ok=sum(r.status_code==200 for r in reads),delete_ok=sum(r.status_code==204 for r in deletes))
    training._manager.shutdown()


if __name__=='__main__':
    with tempfile.TemporaryDirectory(prefix='magnite-extreme-probe-') as directory:
        asyncio.run(main(Path(directory)))
