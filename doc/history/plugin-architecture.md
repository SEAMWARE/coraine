# Plugin architecture - history

How the plugin set came about. What the plugins are today is in
[Plugin architecture](../plugin-architecture.md).

## The DDS bridge: a separate image, then the ordinary one

The DDS bridge links 21.4 MiB over nine libraries against the broker's own 4.28 MiB over
three — which is why it was a separate image for a while, and why it is not one any more:
an image that carries it and never loads it costs that and nothing else.

## MQTT out of the broker

MQTT moved into the `mqtt` bridge plugin, in
[`corMqttBridge`](https://github.com/SEAMWARE/corMqttBridge); the broker itself no longer
links libmosquitto.
