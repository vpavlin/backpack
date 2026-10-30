{
  description = "backpack_core: Basecamp core module that publishes Backpack app repos to Logos Storage (ADR 0011)";

  inputs = {
    # Basecamp 0.3.x: builder 0.3.1 (release tag).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # Update notes go out over Delivery (ADR 0007) through loam_core's shared node; followed repos'
    # notes come back from Store through delivery_module. Pinned to what the 0.3 repo publishes.
    loam_core.url = "github:vpavlin/loam-basecamp/9edb5726122fee745712c758c1096713314552a5?dir=core";
    delivery_module.url = "github:logos-co/logos-delivery-module/v0.3.0";
    loam_core.inputs.delivery_module.follows = "delivery_module";
    # Followers mirror the repos they follow in Basecamp's own Storage node (ADR 0015). The API of the
    # bundled 3.0.2; our 3.0.3 (3.0.2 + libstorage PR #1540) installs over it from our repos.
    storage_module.url = "github:logos-co/logos-storage-module/v3.0.2";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
