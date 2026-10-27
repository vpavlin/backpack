{
  description = "Backpack: pure-QML Basecamp view over the backpack_core module (ADR 0011)";

  inputs = {
    # Same builder as the core (Basecamp 0.3.x).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # A GitHub ref, not path:../backpack_core (a relative path lock breaks a clean clone on some Nix versions).
    # The view only needs the core's interface (LIDL): pin the commit that defines it.
    backpack_core.url = "github:vpavlin/backpack/39b1e7f34166a12e3c29d2dd625c6cc1c706d1d8?dir=basecamp/backpack_core";
    backpack_core.inputs.logos-module-builder.follows = "logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
