### Fixed

- Discovery (`/types`, `/types/{type}`, `/attributes`, `/attributes/{attrId}`) is never forwarded to an auxiliary
  Context Source: TS 104-175 § 4.3.6 limits auxiliary operations to context information consumption. A registration
  without `operations` was forwarded to. The types and attributes it declares are still listed.
