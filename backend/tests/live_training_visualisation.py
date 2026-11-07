"""Opt-in offline CPU LoRA telemetry smoke. Requires backend[ml] and cached tiny-gpt2.

PYTHONPATH=backend/src .venv/bin/python backend/tests/live_training_visualisation.py
Uses disposable datasets/adapters; does not download or modify shared weights.
"""
import json
import os
from pathlib import Path
import tempfile
import threading

os.environ['HF_HUB_OFFLINE']='1'
os.environ['HF_DATASETS_OFFLINE']='1'
os.environ['TOKENIZERS_PARALLELISM']='false'
os.environ['OMP_NUM_THREADS']='2'

from huggingface_hub import snapshot_download
from backend.adapters.store import AdapterStore
from backend.training.schemas import TrainingConfig, TrainingJob
from backend.training.trainer import run_training

with tempfile.TemporaryDirectory(prefix='magnite-training-visualisation-') as directory:
    root=Path(directory)
    os.environ["HF_DATASETS_CACHE"]=str(root/"dataset-cache")
    data=root/'samples.jsonl'
    data.write_text('\n'.join(json.dumps({'text': t}) for t in (
        'Magnite trains small local adapters.', 'Training loss is measured at each step.',
        'Validation uses held out examples.', 'The graph displays raw measurements.')))
    cfg=TrainingConfig(base_model=snapshot_download('sshleifer/tiny-gpt2',local_files_only=True),
        dataset_id='smoke',quantization='none',rank=2,alpha=4,target_modules='all-linear',
        max_length=32,max_steps=6,epochs=2,validation_split=0.25,dropout=0.12)
    job=TrainingJob(id='smoke',name='validation-smoke',status='queued',base_model=cfg.base_model,dataset_id='smoke')
    store=AdapterStore(str(root/'adapters.db'))
    run_training(job,cfg,str(data),str(root/'adapters'),store,threading.Event())
    assert job.status=='done',job.model_dump_json()
    train=[p for p in job.history if 'loss' in p]
    validation=[p for p in job.history if 'eval_loss' in p]
    assert len(train)==6,job.history
    assert len(validation)==2,job.history
    assert all('learning_rate' in p and 'grad_norm' in p and 'elapsed_seconds' in p for p in train),job.history
    assert [p['step'] for p in validation]==[3,6],job.history
    assert job.total_steps==6 and job.current_step==6,job.model_dump_json()
    assert store.get(job.adapter_id) is not None
    if Path('build').is_dir():
        Path('build/training-real-telemetry.json').write_text(job.model_dump_json(indent=2))
    print('PASS real CPU LoRA: 6 training samples, 2 validation samples at steps 3/6, measured LR/gradient/epoch/time, adapter registered.')
