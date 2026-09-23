#pragma once

struct IdentityRegistrationStats
{
  int controllers{0};
  int filters{0};
};

IdentityRegistrationStats registerIdentitySurface();
