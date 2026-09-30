# Example inputs

These small, synthetic examples were authored for Magnite release documentation
on 2026-09-29. They contain fictional observatory facts and no external dataset
material or real personal data. They use the repository's MIT license.

- `training.jsonl`: instruction/input/output records.
- `training.csv`: the same schema as CSV.
- `training.txt`: plain-text examples.
- `observatory.md`: a document for extraction/retrieval and RAG chat.

The rows demonstrate accepted formats only; they are too few to establish useful
fine-tuning quality. Keep any real training/evaluation datasets separate.

Try asking: “When is the observatory library open?” Expected source fact:
Tuesday and Thursday, 18:00–21:00. The maintenance contact is the front desk;
the equipment cabinet is in Room Cedar. Model answers must be checked against
the document, not assumed correct merely because retrieval returned a passage.
