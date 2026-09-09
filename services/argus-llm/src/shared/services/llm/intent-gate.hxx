#pragma once

#include <shared/services/intent/fasttext-classifier.hxx>
#include <shared/services/intent/intent-router.hxx>
#include <shared/services/memory/phrase-catalog.hxx>

#include <string>

// Owns, for the process lifetime, everything the router needs: the phrase
// catalog, the fastText model and the temporal probe that separates a fact
// from a reminder. The probe is built here because it reads the memory
// package's resolver, which argus-intent must never link.
class IntentGate
{
public:
  IntentGate();

  const IntentRouter& router() const { return router_; }
  bool isLoaded() const { return classifier_.isLoaded(); }

private:
  PhraseCatalog catalog_;
  FastTextClassifier classifier_;
  IntentRouter router_;
};
