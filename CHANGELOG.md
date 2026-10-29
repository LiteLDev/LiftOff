# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- The carry spine: Carry On's configuration, lists, per-player carry state, block and entity capture, and the client/server protocol.
- Picking up blocks with their contents, and putting them down with their facing recomputed.
- Picking up mobs, putting them down and stacking them.
- Carrying players.
- Restrictions while carrying: locked hotbar, no item use, containers, breaking or attacking; `dropCarriedWhenHit`.
- Carries survive death (`keepInventory`), logout and dimension changes; `/carryon debug`, `clear` and `place`.
- Carry On scripts and custom pickup conditions.
- The Carry key and the client-side restrictions.
- First-person and third-person rendering of the carried block or mob, and model overrides.
- The carrying arm pose for every client, and a stand-in of the carried block or mob for players without the client mod.

[Unreleased]: https://github.com/LiteLDev/LiftOff/commits/main
