#include <fluxcrawl/version.hpp>

static_assert(fluxcrawl::version == "0.1.0-dev");

int main()
{
    return fluxcrawl::version.empty() ? 1 : 0;
}
