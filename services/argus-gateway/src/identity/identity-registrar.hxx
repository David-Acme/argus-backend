#pragma once

struct IdentityRegistrationStats
{
  int controllers{0};
  int filters{0};
};

// Registers the identity filters and controllers on the running app.
IdentityRegistrationStats registerIdentitySurface();
