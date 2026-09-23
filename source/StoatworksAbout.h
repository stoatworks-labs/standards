/*
 * Stoatworks Labs - About window data for standards.
 *
 * PROVISIONAL HAND COPY, in the shape stoatworks-backend/scripts/sync-about.py
 * generates (adapted from rebate's hand copy on 2026-09-23). The
 * project is not yet registered in the website's projects.json, so the sync
 * cannot produce this file. Once it is registered the sync overwrites this
 * file; edit it there, not here.
 *
 * `guide` is empty on purpose: no user guide exists, and an empty link is left
 * out of the button list rather than shown as a button that opens a 404. The
 * facts were chosen so the button count -- and therefore the parameter count --
 * does not change when this is regenerated.
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "standards";
    inline constexpr auto slug = "standards";
    inline constexpr auto hook = "A field-store standards converter, PAL to NTSC and back, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/standards/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/standards";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
