#pragma once

#include <feature/intent/services/fasttext-classifier.hxx>
#include <feature/intent/services/intent-router.hxx>
#include <phrase/phrase-catalog.hxx>

#include <string>

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
