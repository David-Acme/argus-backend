#pragma once

struct IdentityRegistrationStats
{
  int controllers{0};
  int filters{0};
};

// Registers the identity filters and controllers on the running app and
// returns how many objects were registered.
IdentityRegistrationStats registerIdentitySurface();