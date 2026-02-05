from .client import SpeedHackClient, attach, inject_shared_object

Client = SpeedHackClient

__all__ = ["Client", "SpeedHackClient", "attach", "inject_shared_object"]
