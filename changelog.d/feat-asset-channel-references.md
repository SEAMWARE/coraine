### New

- AssetChannels: a reference to a node of another AssetChannel's model is a Relationship to the entity that
  node is or is in - an AAS ReferenceElement to a shell, a submodel or an element that another AssetChannel
  reads. A reference to a node in an entity (an AAS element), in its own model or another's, is a
  Relationship to that entity with a `targetNode` sub-Property, the node's id; several of one name a
  ListRelationship, `targetNode` a ListProperty, one per object. When another AssetChannel's next model changes
  what such a reference becomes (the node now there, moved, or gone), the AssetChannel with the reference is
  mapped and written again. Before, all of these were Properties, the target's id.
