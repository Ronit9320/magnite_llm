"""Validate portable PEFT artifacts without loading torch or pickle weights."""
import json
from pathlib import Path


def validate_weights(directory):
    root = Path(directory)
    config_path = root / 'adapter_config.json'
    try:
        if config_path.stat().st_size > 1024 * 1024:
            raise ValueError('Adapter configuration exceeds 1 MiB')
        config = json.loads(config_path.read_text(encoding='utf-8'))
        if not isinstance(config, dict) or config.get('peft_type') != 'LORA':
            raise ValueError('Expected a LoRA PEFT configuration')
        if type(config.get('r')) is not int or config['r'] < 1:
            raise ValueError('LoRA configuration requires a positive rank (r)')
        alpha = config.get('lora_alpha')
        if type(alpha) not in (int, float) or alpha <= 0:
            raise ValueError('LoRA configuration requires a positive lora_alpha')
        targets = config.get('target_modules')
        if not ((isinstance(targets, str) and targets.strip()) or
                (isinstance(targets, list) and targets and
                 all(isinstance(t, str) and t.strip() for t in targets))):
            raise ValueError('LoRA configuration requires target_modules')
        weights = root / 'adapter_model.safetensors'
        if not weights.is_file():
            raise ValueError('Safe adapter_model.safetensors weights are required; pickle weights are unsupported')
        from safetensors import safe_open
        with safe_open(str(weights), framework='numpy') as tensors:
            if not list(tensors.keys()):
                raise ValueError('Adapter weights contain no tensors')
    except Exception as exc:
        raise ValueError(f'Invalid adapter files: {exc}') from exc
