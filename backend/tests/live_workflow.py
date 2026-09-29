"""Opt-in real tiny-model regression: registry -> train -> adapter + RAG chat.

Run: PYTHONPATH=backend/src .venv/bin/python backend/tests/live_workflow.py
Requires backend[ml], downloads tiny-gpt2 and the embedding model if uncached.
Uses a fresh temporary MAGNITE_HOME; never deletes shared model weights.
"""
import json
import os
from pathlib import Path
import tempfile
import time

os.environ['MAGNITE_HOME'] = tempfile.mkdtemp(prefix='magnite-live-workflow-')

from fastapi.testclient import TestClient
from huggingface_hub import snapshot_download
from backend.server import app
from backend.routes.models import _get_registry
from backend.models.schemas import ModelRecord


def checked(response, status=200):
    if response.status_code != status:
        raise AssertionError(f'{response.request.url}: {response.status_code}: {response.text}')
    return response.json() if response.content else None


def main():
    with TestClient(app) as client:
        ref = 'sshleifer/tiny-gpt2'
        local = snapshot_download(ref)
        identifier = 'huggingface:sshleifer--tiny-gpt2'
        _get_registry()._store.upsert(ModelRecord(id=identifier, name='tiny-gpt2',
            source='huggingface', ref=ref, path=local))
        result = checked(client.post('/inference/load', json={'model': identifier}))
        assert result['engine']['name'] in ('torch', 'vllm')
        result = checked(client.post('/inference/chat', json={
            'messages':[{'role':'user','content':'Hello'}], 'max_tokens':8, 'stream':False}))
        assert result['choices'][0]['message']['content']
        checked(client.post('/inference/unload'))
        print('PASS registered transformer model -> auto engine -> nonstreaming chat', flush=True)

        samples = [
            {'instruction':'Name the city','input':'Eiffel Tower','output':'Paris'},
            {'text':'The Eiffel Tower is a wrought-iron tower in Paris.'},
            {'instruction':'Name the material','input':'Eiffel Tower','output':'Wrought iron'},
            {'text':'Paris is the capital of France.'},
        ]
        dataset = checked(client.post('/datasets/ingest', json={'name':'mixed.jsonl','format':'jsonl',
            'content':'\n'.join(json.dumps(s) for s in samples)}),201)
        job = checked(client.post('/training/start', json={'base_model':identifier,
            'dataset_id':dataset['id'], 'max_steps':3, 'max_length':64, 'rank':2,
            'alpha':4, 'target_modules':'all-linear','quantization':'none'}),202)
        deadline = time.monotonic()+180
        while time.monotonic()<deadline:
            job=checked(client.get('/training/status/'+job['id']))
            if job['status'] in ('done','error','cancelled'): break
            time.sleep(.2)
        assert job['status']=='done', job
        print('PASS mixed instruction/text training -> registered adapter', flush=True)

        document = checked(client.post('/rag/documents', json={'name':'facts.txt',
            'content':'The Eiffel Tower is a wrought-iron lattice tower in Paris, France.'}),201)
        result=checked(client.post('/rag/query',json={'query':'What is the Eiffel Tower made of?'}))
        assert result['results'][0]['document_id']==document['id']
        checked(client.post('/inference/adapter', json={'adapter_id':job['adapter_id']}))
        result=checked(client.post('/inference/chat',json={'messages':[{'role':'user',
            'content':'What is the Eiffel Tower made of?'}], 'rag':{'enabled':True},
            'max_tokens':8,'stream':False}))
        assert result['choices'][0]['message']['content']
        print('PASS active adapter + real RAG retrieval -> chat response (quality not assessed)',flush=True)
        checked(client.post('/inference/adapter',json={'adapter_id':''}))
        exported=checked(client.post('/adapters/export/'+job['adapter_id']))
        archive=Path(os.environ['MAGNITE_HOME'])/'adapters/exports'/exported['download_path'].rsplit('/',1)[1]
        checked(client.delete('/adapters/'+job['adapter_id']),204)
        checked(client.post('/adapters/import',json={'path':str(archive)}),201)
        checked(client.delete('/rag/documents/'+document['id']),204)
        print('PASS real PEFT export -> delete -> import; RAG cleanup',flush=True)
    with TestClient(app) as client:
        jobs=checked(client.get('/training/jobs'))
        assert any(j['id']==job['id'] and j['status']=='done' for j in jobs)
        print('PASS training history survives backend lifespan restart',flush=True)
    print('Artifacts:',os.environ['MAGNITE_HOME'],flush=True)


if __name__ == '__main__': main()
