# intent.bin

fastText intent classifier, six classes: memory_save, memory_recall,
reminder_set, memory_forget, camera, none. Loaded in-process inside
services/llm as the fast tier of the intent router. `none` means the fast tier
abstains and the LLM decides, tools included.

Routed to a memory tool by the adapter: memory_save, memory_recall, reminder_set, memory_forget. `camera` is a decoy class:
the router reports it, the adapter never routes it, and it exists so camera
asks are not absorbed by memory_recall.

Trained by the intent-training project; this file is the only artifact that
crosses to the backend. Do not edit metrics here - republish.

- config: dim 50, minn 3 / maxn 6, wordNgrams 3, bucket 50000,
  softmax, lr 0.5, epoch 10, thread 1
- operating point: threshold 0.90, margin 0.10
- corpus: 22862 train, 4864 valid, 4921 test rows, 22813
  held-out real negatives, 885 frozen judge rows; splits are disjoint by
  text and by near-duplicate group (token Jaccard 0.65)
- valid precision@1 0.920 (n=4864)
- test gates: Wilson p95lo 0.865 minimum across the five tool classes,
  none recall 0.991 (floor 0.95)
- holdout FPR on 22813 unseen real human negatives:
  none -> memory_save 0.123% (gate 0.25%), any tool 0.697%
  (gate 2%)

## Holdout by source

| source | rows | none -> memory_save | none -> any tool |
|---|---|---|---|
| argus:negatives | 7 | 0.000% | 0.000% |
| argus:test | 322 | 0.311% | 1.553% |
| argus:test2 | 357 | 0.000% | 0.280% |
| argus:train | 2066 | 0.726% | 3.146% |
| argus:valid | 1 | 0.000% | 0.000% |
| external:fleurs-es419 | 494 | 0.000% | 0.000% |
| external:minds14-es | 480 | 0.000% | 0.000% |
| external:multi3nlu-es | 869 | 0.000% | 0.115% |
| external:tatoeba-spa | 1774 | 0.225% | 0.507% |
| massive:dev | 1950 | 0.051% | 0.718% |
| massive:test | 2978 | 0.034% | 0.269% |
| massive:train | 11515 | 0.052% | 0.486% |

## Sources and licenses

- massive: CC-BY-4.0 - https://github.com/alexa/massive (es-ES and en-US, release 1.1)
- argus-legacy: project-internal - data/argus: rescued lab sessions and authored rows of the retired three-class model
- seed: project-internal - data/seed: rows authored for this project, Spanish and English
- judges: project-internal - data/eval: frozen judges, never trained on
- multi3nlu-es: CC-BY-4.0 - https://huggingface.co/datasets/uoe-nlp/multi3-nlu (Spanish, banking and hotels)
- minds14-es: CC-BY-4.0 - https://huggingface.co/datasets/PolyAI/minds14 (es-ES transcriptions of spoken banking requests)
- fleurs-es419: CC-BY-4.0 - https://huggingface.co/datasets/google/fleurs (es_419 dev and test transcriptions)
- tatoeba-spa: CC-BY-2.0-FR - https://tatoeba.org (Spanish sentences, questions and statements without household or scheduling cues)

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
va bien"). See data/gates.json in the intent-training project.

sha256: fcc93bf09984e545da5afb52bbfbaadd6c3f13c4519624a3cd4f7a710267618f
bytes: 12915976
