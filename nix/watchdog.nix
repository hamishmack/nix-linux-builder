# nix-linux-builder — Guest build watchdog
# Copyright 2025 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group
# Apache License 2.0
#
# Builds guest/nlb-watchdog.c as a static binary for the initrd.  The tests
# (tests/guest/watchdog-test.sh) are a separate check, `checks.<system>
# .guest-watchdog`, so their timing cannot fail the initrd build.
{ pkgs, src }:

pkgs.pkgsStatic.stdenv.mkDerivation {
  pname = "nlb-watchdog";
  version = "0.1.0";
  inherit src;

  buildPhase = ''
    runHook preBuild
    $CC -static -O2 -Wall -Wextra -Werror -o nlb-watchdog nlb-watchdog.c
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 nlb-watchdog $out/bin/nlb-watchdog
    runHook postInstall
  '';

  meta = {
    description = "Build-output relay and hang diagnostics for the nix-linux-builder guest";
    license = pkgs.lib.licenses.asl20;
    platforms = pkgs.lib.platforms.linux;
  };
}
