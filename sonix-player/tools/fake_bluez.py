#!/usr/bin/env python3
"""Un finto bluez + finto bluealsa su un vero dbus-daemon.

Serve per provare btstack.c senza il dispositivo: espone org.bluez e
org.bluealsa con le stesse interfacce, gli stessi percorsi e le stesse firme
che usa il player, e si comporta come il vero stack nei punti che contano --
il PCM di bluealsa che compare solo dopo un Connect riuscito, l'agent
richiamato durante il Pair, InterfacesAdded/Removed emessi al momento giusto.

Uso:
    DBUS_SYSTEM_BUS_ADDRESS=... python3 tools/fake_bluez.py

Opzioni via ambiente:
    FAKE_BLUEZ_NO_AGENT=1     non richiama l'agent durante il Pair
    FAKE_BLUEZ_PAIR_FAIL=1    il Pair risponde org.bluez.Error.AuthenticationFailed
    FAKE_BLUEZ_NO_PCM=1       il Connect riesce ma il PCM non compare mai
"""

import os
import sys
import threading
import time

from jeepney import HeaderFields, MessageType, new_error, new_method_call, new_method_return, new_signal
from jeepney import DBusAddress
from jeepney.io.blocking import open_dbus_connection

ADAPTER = "/org/bluez/hci0"

DEVICES = {
    "18:3F:70:73:EE:48": {
        "Alias": "AirPods Pro",
        "Name": "AirPods Pro",
        "Paired": True,
        "Trusted": True,
        "Connected": False,
        "RSSI": -52,
        "UUIDs": ["0000110b-0000-1000-8000-00805f9b34fb", "0000110e-0000-1000-8000-00805f9b34fb"],
    },
    "AA:BB:CC:DD:EE:FF": {
        "Alias": "Sony WH-1000XM4",
        "Name": "Sony WH-1000XM4",
        "Paired": False,
        "Trusted": False,
        "Connected": False,
        "RSSI": -70,
        "UUIDs": ["0000110b-0000-1000-8000-00805f9b34fb"],
    },
    # Senza nome: il player non deve mostrarla nella lista di ricerca.
    "11:22:33:44:55:66": {
        "Alias": "11-22-33-44-55-66",
        "Name": "",
        "Paired": False,
        "Trusted": False,
        "Connected": False,
        "RSSI": -90,
        "UUIDs": [],
    },
}

CODECS = ["SBC", "AAC", "aptX", "LDAC"]

# Una lista lunga, per far uscire la risposta di GetManagedObjects dai 16 KB che
# il client sapeva leggere prima. Con nomi lunghi ci si arriva presto.
_many = int(os.environ.get("FAKE_BLUEZ_MANY", "0"))

# La versione che il finto bluealsa dichiara sul suo Manager1.
FAKE_BLUEALSA_VERSION = os.environ.get("FAKE_BLUEZ_BA_VERSION", "v4.3.1")
for _i in range(_many):
    DEVICES["02:%02X:%02X:%02X:%02X:%02X" % (_i >> 24 & 0xFF, _i >> 16 & 0xFF, _i >> 8 & 0xFF, _i & 0xFF, 0x11)] = {
        "Alias": "Cuffie di prova numero %d con un nome lungo per gonfiare la risposta" % _i,
        "Name": "Cuffie di prova numero %d" % _i,
        "Paired": False,
        "Trusted": False,
        "Connected": False,
        "RSSI": -40 - (_i % 50),
        "UUIDs": ["0000110b-0000-1000-8000-00805f9b34fb", "0000110e-0000-1000-8000-00805f9b34fb",
                  "0000111e-0000-1000-8000-00805f9b34fb"],
    }

adapter_props = {
    "Powered": False,
    "Discoverable": False,
    "Pairable": False,
    "Discovering": False,
    "Alias": "HiBy Music",
    "Address": "4C:BC:98:B0:2A:01",
}

agent_path = None
agent_owner = None
selected_codec = "SBC"
next_serial = 1000


def dev_path(address):
    return ADAPTER + "/dev_" + address.replace(":", "_")


def address_of(path):
    tail = path.rsplit("/dev_", 1)
    if len(tail) != 2:
        return None
    return tail[1][:17].replace("_", ":")


def pcm_path(address):
    return "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink" % address.replace(":", "_")


def variant(sig, value):
    return (sig, value)


# I flussi A2DP che bluez tiene per un dispositivo. Di suo uno solo e attivo;
# FAKE_BLUEZ_TRANSPORTS li detta ("fd5=idle,fd6=active") per riprodurre sia il
# flusso fermo sia il transport orfano che bluez si tiene dopo una
# SetConfiguration rifiutata.
def transports_for(address):
    if not DEVICES[address]["Connected"]:
        return []
    spec = os.environ.get("FAKE_BLUEZ_TRANSPORTS", "fd0=active")
    out = []
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        leaf, _, state = item.partition("=")
        out.append((leaf, state or "active"))
    return out


def device_props(address):
    d = DEVICES[address]
    props = {
        "Address": variant("s", address),
        "Alias": variant("s", d["Alias"]),
        "Paired": variant("b", d["Paired"]),
        "Trusted": variant("b", d["Trusted"]),
        "Connected": variant("b", d["Connected"]),
        "RSSI": variant("n", d["RSSI"]),
        "UUIDs": variant("as", d["UUIDs"]),
        "Adapter": variant("o", ADAPTER),
        "Icon": variant("s", "audio-headset"),
    }
    if d["Name"]:
        props["Name"] = variant("s", d["Name"])
    return props


def adapter_props_dict():
    return {
        "Address": variant("s", adapter_props["Address"]),
        "Alias": variant("s", adapter_props["Alias"]),
        "Powered": variant("b", adapter_props["Powered"]),
        "Discoverable": variant("b", adapter_props["Discoverable"]),
        "Pairable": variant("b", adapter_props["Pairable"]),
        "Discovering": variant("b", adapter_props["Discovering"]),
    }


def pcm_props(address):
    return {
        "Device": variant("o", dev_path(address)),
        "Transport": variant("s", "A2DP-source"),
        "Mode": variant("s", "sink"),
        "Running": variant("b", True),
        "Format": variant("q", 0x8210),
        "Channels": variant("y", 2),
        "Sampling": variant("u", 44100),
        "Codec": variant("s", selected_codec),
        "Volume": variant("q", 0x7F7F),
        "SoftVolume": variant("b", False),
    }


class Fake:
    def __init__(self):
        self.conn = open_dbus_connection(bus="SYSTEM")
        self.pending = {}
        # I segnali ritardati partono da thread di Timer mentre il ciclo
        # principale puo' star rispondendo: due scritture insieme sullo stesso
        # socket si accavallano e il messaggio arriva rotto.
        self.send_lock = threading.Lock()
        self.request_name("org.bluez")
        self.request_name("org.bluealsa")

    def request_name(self, name):
        bus = DBusAddress("/org/freedesktop/DBus", bus_name="org.freedesktop.DBus",
                          interface="org.freedesktop.DBus")
        msg = new_method_call(bus, "RequestName", "su", (name, 0))
        reply = self.conn.send_and_get_reply(msg)
        if reply.body[0] not in (1, 4):
            raise SystemExit("non ho ottenuto %s: %r" % (name, reply.body))

    # --- invio ---

    def send(self, message, **kwargs):
        with self.send_lock:
            self.conn.send(message, **kwargs)

    def reply(self, msg, signature=None, body=()):
        self.send(new_method_return(msg, signature, body))

    def error(self, msg, name, text=""):
        self.send(new_error(msg, name, "s", (text,)))

    def emit(self, path, interface, member, signature, body):
        emitter = DBusAddress(path, bus_name=None, interface=interface)
        self.send(new_signal(emitter, member, signature, body))

    def interfaces_added(self, path, interfaces):
        self.emit("/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                  "oa{sa{sv}}", (path, interfaces))

    def interfaces_removed(self, path, names):
        self.emit("/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                  "oas", (path, names))

    def properties_changed(self, path, interface, changed):
        self.emit(path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                  "sa{sv}as", (interface, changed, []))

    # --- ricezione ---

    def run(self):
        while True:
            try:
                msg = self.conn.receive()
            except Exception:
                return  # il bus e' sparito: e' la fine, non un errore
            if msg.header.message_type in (MessageType.method_return, MessageType.error):
                self.agent_answered(msg)
                continue
            if msg.header.message_type is not MessageType.method_call:
                continue
            try:
                self.dispatch(msg)
            except Exception as exc:  # il finto non deve mai morire in silenzio
                print("fake_bluez: %r" % (exc,), file=sys.stderr, flush=True)
                self.error(msg, "org.bluez.Error.Failed", str(exc))

    # La conferma dell'agent va e torna sulla stessa connessione che riceve
    # tutto il resto, quindi non si puo' aspettare dentro il gestore: il Pair
    # resta in sospeso e viene chiuso quando la risposta arriva qui.
    def ask_agent(self, pair_msg, address):
        global next_serial
        target = DBusAddress(agent_path, bus_name=agent_owner, interface="org.bluez.Agent1")
        call = new_method_call(target, "RequestConfirmation", "ou", (dev_path(address), 123456))
        serial = next_serial
        next_serial += 1
        self.pending[serial] = (pair_msg, address)
        self.send(call, serial=serial)
        threading.Timer(10.0, lambda: self.agent_timeout(serial)).start()

    def agent_timeout(self, serial):
        entry = self.pending.pop(serial, None)
        if entry:
            print("fake_bluez: l'agent non ha risposto", file=sys.stderr, flush=True)
            self.error(entry[0], "org.bluez.Error.AuthenticationCanceled", "agent silent")

    def agent_answered(self, msg):
        serial = msg.header.fields.get(HeaderFields.reply_serial)
        entry = self.pending.pop(serial, None)
        if not entry:
            return
        pair_msg, address = entry
        if msg.header.message_type is MessageType.error:
            self.error(pair_msg, "org.bluez.Error.AuthenticationRejected", "agent said no")
            return
        DEVICES[address]["Paired"] = True
        self.reply(pair_msg)
        self.properties_changed(dev_path(address), "org.bluez.Device1", {"Paired": variant("b", True)})

    def dispatch(self, msg):
        fields = msg.header.fields
        path = fields.get(HeaderFields.path, "")
        iface = fields.get(HeaderFields.interface, "")
        member = fields.get(HeaderFields.member, "")
        dest = fields.get(HeaderFields.destination, "")

        if iface == "org.freedesktop.DBus.ObjectManager" and member == "GetManagedObjects":
            return self.get_managed_objects(msg, dest)
        if iface == "org.freedesktop.DBus.Properties":
            return self.properties(msg, path, member)
        if iface == "org.bluez.AgentManager1":
            return self.agent_manager(msg, member)
        if iface == "org.bluez.Adapter1":
            return self.adapter(msg, member)
        if iface == "org.bluez.Device1":
            return self.device(msg, path, member)
        if iface == "org.bluealsa.PCM1":
            return self.pcm(msg, path, member)
        if iface == "org.freedesktop.DBus.Introspectable":
            return self.reply(msg, "s", ("<node/>",))
        self.error(msg, "org.freedesktop.DBus.Error.UnknownMethod", member)

    def get_managed_objects(self, msg, dest):
        objects = {}
        if dest == "org.bluealsa":
            for address, d in DEVICES.items():
                if d["Connected"] and not os.environ.get("FAKE_BLUEZ_NO_PCM"):
                    objects[pcm_path(address)] = {"org.bluealsa.PCM1": pcm_props(address)}
        else:
            objects[ADAPTER] = {"org.bluez.Adapter1": adapter_props_dict()}
            for address in DEVICES:
                objects[dev_path(address)] = {"org.bluez.Device1": device_props(address)}
                for leaf, state in transports_for(address):
                    objects[dev_path(address) + "/" + leaf] = {
                        "org.bluez.MediaTransport1": {
                            "State": variant("s", state),
                            "Device": variant("o", dev_path(address)),
                            "UUID": variant("s", "0000110a-0000-1000-8000-00805f9b34fb"),
                        }
                    }
        self.reply(msg, "a{oa{sa{sv}}}", (objects,))

    def properties(self, msg, path, member):
        if member == "Get":
            iface, name = msg.body
            props = self.props_for(path, iface)
            if props is None or name not in props:
                return self.error(msg, "org.freedesktop.DBus.Error.InvalidArgs", name)
            self.reply(msg, "v", (props[name],))
        elif member == "GetAll":
            props = self.props_for(path, msg.body[0])
            self.reply(msg, "a{sv}", (props or {},))
        elif member == "Set":
            iface, name, value = msg.body
            if path == ADAPTER and name in adapter_props:
                adapter_props[name] = value[1]
                self.reply(msg)
                self.properties_changed(path, iface, {name: value})
            elif "/dev_" in path and name == "Trusted":
                DEVICES[address_of(path)]["Trusted"] = value[1]
                self.reply(msg)
            else:
                self.error(msg, "org.freedesktop.DBus.Error.PropertyReadOnly", name)
        else:
            self.error(msg, "org.freedesktop.DBus.Error.UnknownMethod", member)

    def props_for(self, path, iface):
        if path == ADAPTER and iface == "org.bluez.Adapter1":
            return adapter_props_dict()
        if "/dev_" in path and iface == "org.bluez.Device1":
            address = address_of(path)
            return device_props(address) if address in DEVICES else None
        if path.startswith("/org/bluealsa/") and iface == "org.bluealsa.PCM1":
            return pcm_props(address_of(path))
        # Il manager di bluealsa: qui serve solo la Version, che e' come il
        # player distingue il 4.1.1 di serie dal 4.3.1 che spediamo noi.
        if path == "/org/bluealsa" and iface == "org.bluealsa.Manager1":
            return {"Version": variant("s", FAKE_BLUEALSA_VERSION)}
        return None

    def agent_manager(self, msg, member):
        global agent_path, agent_owner
        if member == "RegisterAgent":
            agent_path = msg.body[0]
            agent_owner = msg.header.fields.get(HeaderFields.sender)
            print("fake_bluez: agent %s da %s" % (agent_path, agent_owner), flush=True)
            self.reply(msg)
        elif member == "RequestDefaultAgent":
            self.reply(msg)
        elif member == "UnregisterAgent":
            agent_path = None
            self.reply(msg)
        else:
            self.error(msg, "org.freedesktop.DBus.Error.UnknownMethod", member)

    def adapter(self, msg, member):
        if member == "StartDiscovery":
            if adapter_props["Discovering"]:
                return self.error(msg, "org.bluez.Error.InProgress", "already discovering")
            adapter_props["Discovering"] = True
            self.reply(msg)
            self.properties_changed(ADAPTER, "org.bluez.Adapter1", {"Discovering": variant("b", True)})
        elif member == "StopDiscovery":
            if not adapter_props["Discovering"]:
                return self.error(msg, "org.bluez.Error.Failed", "not discovering")
            adapter_props["Discovering"] = False
            self.reply(msg)
        elif member == "RemoveDevice":
            address = address_of(msg.body[0])
            if address not in DEVICES:
                return self.error(msg, "org.bluez.Error.DoesNotExist", "no such device")
            DEVICES[address]["Paired"] = False
            DEVICES[address]["Trusted"] = False
            self.reply(msg)
            self.interfaces_removed(dev_path(address), ["org.bluez.Device1"])
        else:
            self.error(msg, "org.freedesktop.DBus.Error.UnknownMethod", member)

    # I passaggi di stato del flusso A2DP, uno alla volta. Chi li ascolta deve
    # star girando: un client che dorme senza leggere riempie la coda del bus e
    # questo thread si ferma sulla scrittura.
    def transport_states(self, address):
        for state in ("pending", "active", "idle"):
            self.properties_changed(dev_path(address) + "/fd0", "org.bluez.MediaTransport1",
                                    {"State": variant("s", state)})
            time.sleep(0.3)

    def device(self, msg, path, member):
        address = address_of(path)
        if address not in DEVICES:
            return self.error(msg, "org.bluez.Error.DoesNotExist", "no such device")
        d = DEVICES[address]

        if member == "Pair":
            if os.environ.get("FAKE_BLUEZ_PAIR_FAIL"):
                return self.error(msg, "org.bluez.Error.AuthenticationFailed", "refused")
            # Il vero bluez chiede conferma all'agent prima di rispondere. Qui
            # succede la stessa cosa, su un thread a parte perche' la risposta
            # dell'agent arriva su questa stessa connessione.
            if os.environ.get("FAKE_BLUEZ_NO_AGENT") or not agent_path:
                # Nessun agent registrato: bluez non ha nessuno a cui chiedere la
                # conferma e la richiesta muore cosi'.
                return self.error(msg, "org.bluez.Error.AuthenticationCanceled", "no agent")
            self.ask_agent(msg, address)

        elif member == "Connect":
            d["Connected"] = True
            self.reply(msg)
            self.properties_changed(path, "org.bluez.Device1", {"Connected": variant("b", True)})
            if not os.environ.get("FAKE_BLUEZ_NO_PCM"):
                # Il PCM di bluealsa compare dopo, non insieme: e' esattamente la
                # distinzione fra "link su" e "audio pronto".
                threading.Timer(0.4, lambda: self.interfaces_added(
                    pcm_path(address), {"org.bluealsa.PCM1": pcm_props(address)})).start()
                # E il flusso A2DP che cambia stato: e' il segnale con cui bluez
                # dice se quello che l'encoder manda viene davvero riprodotto.
                # Uno alla volta e da un thread solo: due scritture insieme sullo
                # stesso socket si accavallano.
                threading.Timer(0.6, lambda: self.transport_states(address)).start()

        elif member == "Disconnect":
            was = d["Connected"]
            d["Connected"] = False
            self.reply(msg)
            self.properties_changed(path, "org.bluez.Device1", {"Connected": variant("b", False)})
            if was and not os.environ.get("FAKE_BLUEZ_NO_PCM"):
                self.interfaces_removed(pcm_path(address), ["org.bluealsa.PCM1"])
        else:
            self.error(msg, "org.freedesktop.DBus.Error.UnknownMethod", member)

    def pcm(self, msg, path, member):
        global selected_codec
        address = address_of(path)
        if (address not in DEVICES or not DEVICES[address]["Connected"]
                or os.environ.get("FAKE_BLUEZ_NO_PCM")):
            return self.error(msg, "org.freedesktop.DBus.Error.UnknownObject", path)
        if member == "GetCodecs":
            self.reply(msg, "a{sa{sv}}", ({name: {} for name in CODECS},))
        elif member == "SelectCodec":
            name = msg.body[0]
            if name not in CODECS:
                return self.error(msg, "org.bluez.Error.NotSupported", name)
            selected_codec = name
            self.reply(msg)
            self.properties_changed(path, "org.bluealsa.PCM1", {"Codec": variant("s", name)})
        else:
            self.error(msg, "org.freedesktop.DBus.Error.UnknownMethod", member)


if __name__ == "__main__":
    fake = Fake()
    print("fake_bluez: pronto", flush=True)
    fake.run()
