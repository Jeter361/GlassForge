# Contributing

1. Keep process targeting explicit; never fall back to matching arbitrary window-title text.
2. Preserve per-monitor DPI v2 behavior.
3. Avoid expensive work in movement and focus event callbacks.
4. Add a smoke or integration test for behavioral changes.
5. Run `dotnet build GlassForge.sln` and the smoke-test project before committing.

Use focused commits and document any Windows build-specific behavior in the pull request.

