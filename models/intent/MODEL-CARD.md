# intent.bin

fastText intent classifier, six classes: memory_save, memory_recall,
reminder_set, memory_forget, camera, none. Loaded in-process inside
services/argus-llm as the fast tier of the intent router.

Trained by the intent-training project; this file is the only artifact that
crosses to the backend. Do not edit metrics here - republish.

- config: dim 50, minn 3 / maxn 6, wordNgrams 3, bucket 50000, softmax,
  lr 0.5, epoch 10, thread 1
- operating point: threshold 0.90, margin 0.10 (margin is inert at this
  threshold, kept for config-shape continuity)
- valid precision@1 0.913 (n=5205)
- test gates: Wilson p95lo 0.875 minimum across classes
  (floors 0.85, memory_forget 0.80), none recall 0.991 (floor 0.95)
- holdout FPR on 9934 unseen real human negatives:
  none -> memory_save 19.100% (gate 0.25%), any tool 105.700%
  (gate 2%)

## Why not a .ftz

Every quantized configuration (cutoff 10k/30k/50k/210k/400k, retrain
on/off) was gated: argmax falls ~0.913 -> ~0.88 and the thin-class Wilson
precision floors collapse to ~0.6-0.7, because quantization noise perturbs
scores around the 0.90 deployment threshold. The shipped .bin is the size
fix: bucket 200000 -> 50000 alone took the model 42 MB -> 12.6 MB with no
gate loss. Revisit quantization only with a re-swept operating point
measured on the .ftz itself.

## Why the ms-FPR gate is 0.25%

0.1% is statistically unverifiable at n~10k (Wilson interval 0.05-0.37%),
and the audited residual contains utterances the Argus taxonomy genuinely
saves that MASSIVE labels none (a stated wifi password, "anota que mi dia
va bien"). See scripts/evaluate.py.

sha256: 2d74a59f1aacae06d38f5465310a4674e1f63dce86ba88911571168ed5dc4418
bytes: 12647335
