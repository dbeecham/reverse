{

  inputs = {
    # updated 2022-02-07
    nixpkgs = {
      type = "github";
      owner = "NixOS";
      repo = "nixpkgs";
      rev = "a102368ac4c3944978fecd9d7295a96d64586db5";
      narHash = "sha256-hgdcyLo2d8N2BmHuPMWhsXlorv1ZDkhBjq1gMYvFbdo=";
    };
  };


  outputs = { self, nixpkgs }: {

    defaultPackage.x86_64-linux = nixpkgs.legacyPackages.x86_64-linux.stdenv.mkDerivation {
      name = "reverse";
      src = self;
      installFlags = [ "DESTDIR=$(out)" "PREFIX=/" ];
      depsBuildBuild = [
        nixpkgs.legacyPackages.x86_64-linux.kconfig-frontends
      ];
    };

  };

}
