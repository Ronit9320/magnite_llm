"""Service failure paths and adapter output ownership regressions."""
from pathlib import Path
import tempfile
import threading
import types
import unittest
from unittest.mock import MagicMock, patch

from backend.adapters.store import AdapterStore
from backend.models.registry import ModelRegistry
from backend.models.store import ModelStore
from backend.training.schemas import TrainingConfig, TrainingJob
from backend.training.trainer import run_training


class PullJobTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.registry = ModelRegistry(ModelStore(str(self.root / 'models.db')), str(self.root / 'hub'))

    def test_hf_pull_success_registers_model_and_completes_job(self):
        path = self.root / 'snapshot'
        path.mkdir()
        (path / 'config.json').write_text('{"architectures":["TestModel"]}')
        job_id = self.registry._create_job('org/test', 'huggingface')
        info = types.SimpleNamespace(cardData={'license': 'mit'}, downloads=2, likes=1, tags=['test'])
        with patch.object(self.registry, '_hf_snapshot_path', return_value=str(path)), \
             patch.object(self.registry._api, 'model_info', return_value=info):
            self.registry._run_hf_pull('org/test', job_id)
        self.assertEqual(self.registry.job_status(job_id).status, 'done')
        self.assertEqual(self.registry.job_status(job_id).progress, 100)
        self.assertEqual(self.registry.get_model('huggingface:org--test').license, 'mit')

    def test_failed_pull_is_reported_without_registering_model(self):
        job_id = self.registry._create_job('org/test', 'huggingface')
        with patch.object(self.registry, '_hf_snapshot_path', side_effect=OSError('disk full')):
            self.registry._run_hf_pull('org/test', job_id)
        self.assertEqual(self.registry.job_status(job_id).status, 'error')
        self.assertIn('disk full', self.registry.job_status(job_id).error)
        self.assertIsNone(self.registry.get_model('huggingface:org--test'))
        self.assertIsNone(self.registry.job_status('missing'))

    def test_hf_metadata_failure_leaves_download_discoverable(self):
        # A completed download must become visible on refresh even if Hub
        # metadata fails after the bytes have arrived.
        path = self.root / 'hub/models--org--test/snapshots/abc'
        path.mkdir(parents=True)
        (path / 'config.json').write_text('{}')
        (path / 'model.safetensors').write_text('weights')
        job_id = self.registry._create_job('org/test', 'huggingface')
        with patch.object(self.registry, '_hf_snapshot_path', return_value=str(path)), \
             patch.object(self.registry._api, 'model_info', side_effect=OSError('offline')):
            self.registry._run_hf_pull('org/test', job_id)
        self.assertEqual(self.registry.job_status(job_id).status, 'error')
        self.assertEqual([r.ref for r in self.registry.list_synced(source='huggingface')], ['org/test'])


class TrainingOutputTests(unittest.TestCase):
    def test_names_cannot_escape_or_overwrite_another_jobs_weights(self):
        # Exercise the real trainer save/registration path, replacing only ML.
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            store = AdapterStore(str(root / 'adapters.db'))
            output = root / 'adapters'
            output.mkdir()
            model = MagicMock()
            model.config.architectures = ['TestModel']
            def save(path):
                Path(path, 'adapter_model.safetensors').write_text(Path(path).name)
            model.save_pretrained.side_effect = save
            trainer = MagicMock()
            trainer.train.return_value = types.SimpleNamespace(global_step=1, training_loss=1.)
            ml = {name: MagicMock() for name in ('torch', 'AutoModelForCausalLM', 'AutoTokenizer',
                'BitsAndBytesConfig', 'DataCollatorForLanguageModeling', 'TrainingArguments', 'Trainer')}
            ml['TrainerCallback'] = object
            ml['torch'].cuda.is_available.return_value = False
            ml['AutoModelForCausalLM'].from_pretrained.return_value = model
            ml['Trainer'].return_value = trainer
            peft = types.ModuleType('peft')
            peft.LoraConfig = MagicMock()
            peft.get_peft_model = MagicMock(return_value=model)
            peft.prepare_model_for_kbit_training = MagicMock(return_value=model)
            datasets = types.ModuleType('datasets')
            datasets.Dataset = MagicMock()
            with patch('backend.training.trainer._require_ml', return_value=ml), \
                 patch('backend.training.trainer._tokenize_jsonl', return_value=[{'input_ids': [1]}]), \
                 patch.dict('sys.modules', {'peft': peft, 'datasets': datasets}):
                for i, name in enumerate(('../escaped', str(root / 'absolute'), 'same', 'same')):
                    cfg = TrainingConfig(base_model='test/base', dataset_id='data', name=name,
                                         max_steps=1, target_modules='all-linear')
                    job = TrainingJob(id=str(i), name=name, status='queued', base_model='test/base', dataset_id='data')
                    run_training(job, cfg, 'unused.jsonl', str(output), store, threading.Event())
                    self.assertEqual(job.status, 'done', job.error)
                    rec = store.get(job.adapter_id)
                    self.assertEqual(rec.name, name)
                    self.assertTrue(Path(rec.path).is_relative_to(output))
            records = store.list()
            self.assertEqual(len({rec.path for rec in records}), 4)
            self.assertFalse((root / 'escaped').exists())
            self.assertFalse((root / 'absolute').exists())
            for rec in records:
                self.assertEqual(Path(rec.path, 'adapter_model.safetensors').read_text(), Path(rec.path).name)


class BaseStartupTests(unittest.TestCase):
    def test_startup_and_missing_ml_errors_without_optional_packages(self):
        import os
        import subprocess
        import sys
        code = '''
import importlib.abc, importlib.util, sys, time
class MissingML(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname.split('.')[0] in {'torch', 'transformers', 'peft', 'bitsandbytes',
                'sentence_transformers', 'datasets', 'vllm', 'llama_cpp'}:
            raise ImportError('ML dependency intentionally unavailable')
sys.meta_path.insert(0, MissingML())
# An absent package makes find_spec return None; an actual import raises.
# vLLM probes via find_spec rather than importing the package.
original_find_spec = importlib.util.find_spec
importlib.util.find_spec = lambda name, *args: None if name == 'vllm' else original_find_spec(name, *args)
from fastapi.testclient import TestClient
from backend.server import app
with TestClient(app) as client:
    assert client.get('/health').status_code == 200
    assert client.get('/inference/status').json()['loaded_model'] is None
    assert client.get('/rag/status').json()['engine_ready'] is False
    assert client.post('/rag/documents', json={'name':'x', 'content':'hello'}).status_code == 503
    dataset = client.post('/datasets/ingest', json={'name':'x', 'content':'hello'}).json()
    response = client.post('/training/start', json={'base_model':'test/base', 'dataset_id':dataset['id']})
    assert response.status_code == 202, response.text
    job = response.json()
    deadline = time.monotonic() + 5
    while job['status'] in ('queued', 'running') and time.monotonic() < deadline:
        time.sleep(.01)
        job = client.get('/training/status/' + job['id']).json()
    assert job['status'] == 'error', job
    assert 'ML extras' in job['error'], job
print('PASS')
'''
        with tempfile.TemporaryDirectory() as root:
            env = {**os.environ, 'MAGNITE_HOME': root, 'HF_HUB_CACHE': str(Path(root) / 'hub'),
                   'PYTHONPATH': str(Path(__file__).resolve().parents[1] / 'src')}
            result = subprocess.run([sys.executable, '-c', code], env=env,
                                    text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS', result.stdout)
