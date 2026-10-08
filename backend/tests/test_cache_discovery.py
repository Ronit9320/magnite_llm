"""Offline Hub cache discovery, using real Hub snapshot/symlink layouts."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import httpx
from huggingface_hub.errors import CacheNotFound

from backend.models.registry import ModelRegistry
from backend.models.schemas import ModelRecord, model_id
from backend.models.store import ModelStore
from backend.routes import models
from backend.server import app


class CacheDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cache = self.root / 'hub'
        self.cache.mkdir()
        self.store = ModelStore(str(self.root / 'models.db'))
        self.registry = ModelRegistry(self.store, str(self.cache))
        network = patch.object(self.registry._api, 'model_info', side_effect=AssertionError('Network used'))
        network.start()
        self.addCleanup(network.stop)

    def snapshot(self, repo='org/model', revision='abc123', files=None, ref='main', kind='models'):
        repo_path = self.cache / (kind + '--' + repo.replace('/', '--'))
        path = repo_path / 'snapshots' / revision
        path.mkdir(parents=True)
        blobs = repo_path / 'blobs'
        blobs.mkdir(exist_ok=True)
        if files is None:
            files = {'config.json': '{"architectures":["TestForCausalLM"]}', 'model.safetensors': 'weights'}
        for i, (name, content) in enumerate(files.items()):
            blob = blobs / f'{revision}-{i}'
            blob.write_text(content)
            dest = path / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.symlink_to(os.path.relpath(blob, dest.parent))
        if ref:
            refs = repo_path / 'refs' / ref
            refs.parent.mkdir(parents=True, exist_ok=True)
            refs.write_text(revision)
        return path

    def test_external_cache_is_discovered_offline_and_filtered(self):
        path = self.snapshot()
        self.snapshot('org/other')
        records = self.registry.list_synced(source='huggingface', q='org/model')
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0].path, str(path))
        self.assertEqual(records[0].id, model_id('huggingface', 'org/model'))
        self.assertEqual(records[0].architecture, 'TestForCausalLM')
        self.assertEqual(records, self.registry.list_synced(source='huggingface', q='org/model'))

    def test_main_preferred_and_existing_revision_metadata_preserved(self):
        main = self.snapshot()
        other = self.snapshot(revision='other', ref='branch')
        rec = self.registry.list_synced(source='huggingface')[0]
        self.assertEqual(rec.path, str(main))
        rec.path = str(other)
        rec.license = 'test-license'
        rec.meta = {'likes': 12}
        rec.downloaded_at = '2020-01-01T00:00:00Z'
        self.store.upsert(rec)
        result = self.registry.list_synced(source='huggingface')[0]
        self.assertEqual(result.path, str(other))
        self.assertEqual(result.license, rec.license)
        self.assertEqual(result.meta['likes'], 12)
        self.assertEqual(result.downloaded_at, rec.downloaded_at)

    def test_missing_registered_revision_replaced_by_cached_revision(self):
        path = self.snapshot()
        self.store.upsert(ModelRecord(id=model_id('huggingface', 'org/model'), name='kept',
                                     source='huggingface', ref='org/model', path='/missing'))
        result = self.registry.list_synced(source='huggingface')[0]
        self.assertEqual(result.path, str(path))
        self.assertEqual(result.name, 'kept')

    def test_shards_require_all_index_entries_and_gguf_needs_no_config(self):
        path = self.snapshot(files={'config.json': '{}', 'model-1.safetensors': 'weights',
            'model.safetensors.index.json': json.dumps({'weight_map': {'a': 'model-1.safetensors', 'b': 'model-2.safetensors'}})})
        self.assertEqual(self.registry.list_synced(source='huggingface'), [])
        (path / 'model-2.safetensors').write_text('weights')
        self.snapshot('org/gguf', files={'nested/model.gguf': 'gguf weights'})
        self.assertEqual(len(self.registry.list_synced(source='huggingface')), 2)

    def test_partial_adapter_and_dataset_snapshots_are_not_base_models(self):
        self.snapshot('org/tokenizer', files={'config.json': '{}', 'tokenizer.json': '{}'})
        self.snapshot('org/adapter', files={'adapter_config.json': '{}', 'adapter_model.safetensors': 'weights'})
        self.snapshot('org/empty', files={'config.json': '{}', 'model.safetensors': ''})
        self.snapshot('org/data', kind='datasets')
        self.assertEqual(self.registry.list_synced(source='huggingface'), [])

    def test_broken_repo_does_not_hide_healthy_repo(self):
        self.snapshot('org/good')
        path = self.snapshot('org/broken')
        (path / 'model.safetensors').unlink()
        (path / 'model.safetensors').symlink_to('../../blobs/missing')
        with self.assertLogs('backend.models.registry', level='WARNING'):
            records = self.registry.list_synced(source='huggingface')
        self.assertEqual([r.ref for r in records], ['org/good'])

    def test_missing_cache_and_invalid_config_are_tolerated(self):
        registry = ModelRegistry(self.store, str(self.root / 'missing'))
        self.assertEqual(registry.list_synced(source='huggingface'), [])
        self.snapshot(files={'config.json': '[]', 'model.safetensors': 'weights'})
        self.assertEqual(self.registry.list_synced(source='huggingface')[0].architecture, '')

    def test_cache_removed_during_scan_keeps_registered_records(self):
        self.snapshot()
        before = self.registry.list_synced(source='huggingface')
        with patch('backend.models.registry.scan_cache_dir',
                   side_effect=CacheNotFound('removed', cache_dir=self.cache)), \
             self.assertLogs('backend.models.registry', level='WARNING'):
            self.assertEqual(self.registry.list_synced(source='huggingface'), before)

    def test_hf_source_filter_never_contacts_ollama(self):
        self.snapshot()
        with patch.object(self.registry, 'list_ollama', side_effect=AssertionError('Ollama used')):
            self.assertEqual(len(self.registry.list_synced(source='huggingface')), 1)

    def test_cache_environment_precedence_at_process_start(self):
        code = ('import tempfile; from backend.routes.models import _registry_setup, _get_registry; '
                '_registry_setup(); print(_get_registry()._hf_cache)')
        base = {k: v for k, v in os.environ.items()
                if k not in ('HF_HOME', 'HF_HUB_CACHE', 'HUGGINGFACE_HUB_CACHE', 'XDG_CACHE_HOME')}
        base['MAGNITE_HOME'] = str(self.root / 'app')
        base['PYTHONPATH'] = str(Path(__file__).resolve().parents[1] / 'src')
        scenarios = [
            ({'HF_HOME': str(self.root / 'hf')}, self.root / 'hf/hub'),
            ({'XDG_CACHE_HOME': str(self.root / 'xdg')}, self.root / 'xdg/huggingface/hub'),
            ({'HF_HOME': str(self.root / 'hf'), 'HUGGINGFACE_HUB_CACHE': str(self.root / 'legacy')}, self.root / 'legacy'),
            ({'HF_HOME': str(self.root / 'hf'), 'HUGGINGFACE_HUB_CACHE': str(self.root / 'legacy'),
              'HF_HUB_CACHE': str(self.cache)}, self.cache),
        ]
        for overrides, expected in scenarios:
            with self.subTest(overrides=overrides):
                result = subprocess.run([sys.executable, '-c', code], env={**base, **overrides},
                                        text=True, capture_output=True, timeout=30, check=True)
                self.assertEqual(result.stdout.strip(), str(expected))

    def test_discovered_snapshot_deletion_only_affects_disposable_cache(self):
        path = self.snapshot()
        rec = self.registry.list_synced(source='huggingface')[0]
        self.assertTrue(self.registry.delete_model(rec.id))
        self.assertFalse(path.exists())
        self.assertEqual(self.registry.list_synced(source='huggingface'), [])


class CacheApiTests(unittest.IsolatedAsyncioTestCase):
    async def test_list_endpoint_registers_external_model_for_inference_resolution(self):
        fixture = CacheDiscoveryTests()
        fixture.setUp()
        try:
            path = fixture.snapshot()
            with patch.object(models, '_registry', fixture.registry):
                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url='http://test') as client:
                    response = await client.get('/models', params={'source': 'huggingface'})
                    self.assertEqual(response.status_code, 200)
                    identifier = response.json()[0]['id']
                    self.assertEqual(fixture.registry.get_model(identifier).path, str(path))
        finally:
            fixture.doCleanups()
