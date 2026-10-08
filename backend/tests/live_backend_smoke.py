"""Offline process/HTTP smoke test using the already cached tiny GGUF.

Run with PYTHONPATH=backend/src .venv/bin/python backend/tests/live_backend_smoke.py.
Never pulls or deletes shared model weights. Requires the cached
aladar/tiny-random-LlamaForCausalLM-GGUF model and llama-cpp-python.
"""
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time

import httpx


def main():
    with tempfile.TemporaryDirectory(prefix='magnite-http-smoke-') as root:
        log = Path(root) / 'backend.log'
        env = {**os.environ, 'MAGNITE_HOME': str(Path(root) / 'data'),
               'HF_HUB_OFFLINE': '1', 'TRANSFORMERS_OFFLINE': '1'}
        process = subprocess.Popen([sys.executable, '-m', 'backend.main', '--port', '0'],
                                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        lines = queue.Queue()
        def collect():
            with log.open('w') as output:
                for line in process.stderr:
                    output.write(line)
                    output.flush()
                    lines.put(line)
        reader = threading.Thread(target=collect, daemon=True)
        reader.start()
        try:
            deadline = time.monotonic() + 90
            url = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise AssertionError(log.read_text())
                try:
                    match = re.search(r'starting on (http://127\.0\.0\.1:\d+)', lines.get(timeout=.5))
                    if match:
                        url = match[1]
                        break
                except queue.Empty:
                    pass
            assert url, 'Backend did not advertise a port'
            with httpx.Client(base_url=url, timeout=60, trust_env=False) as client:
                while time.monotonic() < deadline:
                    try:
                        if client.get('/health').status_code == 200:
                            break
                    except httpx.HTTPError:
                        pass
                    time.sleep(.1)
                def checked(method, route, status=200, **kwargs):
                    result = client.request(method, route, **kwargs)
                    assert result.status_code == status, (route, result.status_code, result.text)
                    return result.json() if result.content else None
                for route in ('/health', '/openapi.json', '/inference/status', '/training/jobs',
                              '/datasets', '/adapters', '/rag/config', '/rag/documents', '/rag/status'):
                    checked('GET', route)
                print('PASS random-port process startup, health, OpenAPI and all read route groups', flush=True)
                records = checked('GET', '/models?source=huggingface')
                identifier = 'huggingface:aladar--tiny-random-LlamaForCausalLM-GGUF'
                assert any(r['id'] == identifier for r in records), 'Required tiny GGUF is not cached'
                result = checked('POST', '/inference/load', json={'model': identifier, 'engine': 'llamacpp'})
                assert result['engine']['name'] == 'llamacpp'
                result = checked('POST', '/inference/completion', json={
                    'prompt': 'Hello', 'max_tokens': 8, 'temperature': 0, 'stream': False})
                assert result['choices'][0]['text']
                with client.stream('POST', '/inference/chat', json={
                    'messages': [{'role': 'user', 'content': 'Hello'}], 'max_tokens': 8}) as response:
                    assert response.status_code == 200, response.read()
                    events = [json.loads(line[5:].strip()) for line in response.iter_lines() if line.startswith('data:')]
                assert events[-1] == {'done': True}, events
                assert any(event.get('delta') for event in events), events
                checked('POST', '/inference/unload')
                assert checked('GET', '/inference/status')['loaded_model'] is None
                print('PASS discovered GGUF -> llama.cpp load -> completion + chat SSE -> unload', flush=True)
                dataset = checked('POST', '/datasets/ingest', status=201,
                                  json={'name': 'smoke', 'format': 'csv', 'content': 'text\nhello world\n'})
                checked('GET', '/datasets/' + dataset['id'])
                checked('DELETE', '/datasets/' + dataset['id'], status=204)
                checked('GET', '/datasets/' + dataset['id'], status=404)
                checked('POST', '/inference/load', status=422, json={'model': 'x', 'engine': 'bad'})
                checked('GET', '/training/status/missing', status=404)
                checked('GET', '/models/pull/missing', status=404)
                print('PASS dataset lifecycle and HTTP validation/error responses', flush=True)
        except Exception:
            print(log.read_text(), file=sys.stderr)
            raise
        finally:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            reader.join(timeout=2)
            process.stderr.close()
        assert process.returncode in (0, -15), process.returncode
        assert 'Application shutdown complete' in log.read_text(), log.read_text()
        print('PASS graceful backend shutdown', flush=True)


if __name__ == '__main__':
    main()
