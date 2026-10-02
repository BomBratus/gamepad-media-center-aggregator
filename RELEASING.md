# PS4 Stremio-only release workflow

The supported delivery is the PS4 Stremio-only package. The Linux build is an
opt-in shared-behavior test bench; it is not a release target. Release identity,
version and content ID live in `CMakeLists.txt` and `cmake/gmca_ps4_pkg.cmake`.

The authorized GitHub Actions workflow builds and validates the versioned PS4
package, publishes the stable `GMCA-PS4-Stremio-only.pkg` artifact, checksums,
and rolling `ps4-update.json` manifest. The updater contract requires schema 1,
channel `ps4-stremio-only`, title ID `GMCA00000`, matching content ID/version,
the expected versioned package filename, exact size, and SHA-256. The manifest
is published after the package and checksums so clients never consume a
manifest for an artifact that has not arrived yet. The in-app updater downloads
the verified package to `/data/pkg` and asks the user to install it through the
GoldHEN Package Installer.

Before changing release source, run the affected local checks and review the
generated package and manifest contract. Linux behavior checks use the explicit
`GMCA_LINUX_TEST_BENCH=ON` profile and no more than two build jobs. A local test
run does not publish anything. Do not treat this document as authorization to
run remote workflows, publish releases, update firmware, or install a package
on a console.

Historical changelog sections and contributor attribution remain authoritative
for earlier releases; they do not describe the current delivery profile.
