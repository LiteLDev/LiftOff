# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-10-29

First release: the behaviour of Carry On 2.11.2 for LeviLamina, on dedicated servers and on the client.

### Added

- Pick up blocks with their contents (chests, furnaces, ...) and put them down with their facing recomputed.
- Pick up mobs, put them down and stack them; carry players.
- Slowness by weight, and the restrictions while carrying: locked hotbar, no item use, no containers, no breaking
  or attacking; `dropCarriedWhenHit`.
- Carries survive death (`keepInventory`), logout and dimension changes.
- Commands `/carryon debug`, `clear [targets]`, `place [targets]`, `reload`.
- Carry On's configuration (`config/carryon.json`, `config/carryon_client.json`) with its default lists,
  scripts (`config/scripts/`) and custom pickup conditions.
- The Carry key (default Left Shift) and the client-side restrictions.
- First-person and third-person rendering of the carried block or mob, model overrides, `facePlayer`,
  `rotateEntitiesSideways`, and the carrying arm pose (`renderArms`, per viewer).
- Client settings apply live while playing on a dedicated server.
- Players without the client mod see the arm pose and a stand-in of the carried block or mob.
- English and Simplified Chinese.

[Unreleased]: https://github.com/LiteLDev/LiftOff/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/LiteLDev/LiftOff/releases/tag/v0.1.0
