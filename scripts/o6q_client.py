#!/usr/bin/env python3
"""Talk to o6q_server on QNX with an independent OPC UA stack (python asyncua).

    python scripts/o6q_client.py URL --cert CERT.der --key KEY.pem
                                 --user NAME:PASSWORD [--seconds 15]
    python scripts/o6q_client.py --make-cert CERT.der KEY.pem

The server is expected to run with --secure, --trust (a directory that holds
CERT.der) and --user (see vm/run.sh). The client checks:

  host.endpoints        GetEndpoints lists the security policies
  host.anonymous        an anonymous session is refused (--user is set)
  host.browse_read      Basic256Sha256 SignAndEncrypt + user name: browse
                        Objects/QNX and read every variable
  host.subscription     data changes of Uptime and CpuLoadAverage arrive
  host.<policy>         the other policies the server and asyncua share
  host.wrong_password   a wrong password is refused
  host.untrusted_cert   a client certificate that is not in the trust
                        directory is refused

Each result is one line "O6Q PASS|FAIL host.<test> ...". The exit status is 1
if a test failed. Needs: pip install asyncua
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import asyncio
import datetime
import os
import sys
import tempfile
import time

NAMESPACE_URI = "urn:open62541-qnx:system"
CLIENT_URI = "urn:open62541-qnx:host-client"
TIMEOUT = 60  # seconds per request; RSA on an emulated CPU is slow

failures = 0


def report(ok, name, text):
    global failures
    if not ok:
        failures += 1
    print(f"O6Q {'PASS' if ok else 'FAIL'} host.{name} {text}", flush=True)


def make_cert(cert_path, key_path, common_name="o6q host client"):
    """A self-signed application instance certificate (RSA 2048, SHA-256)."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name),
                      x509.NameAttribute(NameOID.ORGANIZATION_NAME, "open62541-qnx")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder()
            .subject_name(name).issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=365))
            .add_extension(x509.SubjectAlternativeName(
                [x509.UniformResourceIdentifier(CLIENT_URI), x509.DNSName("localhost")]),
                critical=False)
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.KeyUsage(
                digital_signature=True, content_commitment=True, key_encipherment=True,
                data_encipherment=True, key_agreement=False, key_cert_sign=False,
                crl_sign=False, encipher_only=False, decipher_only=False), critical=True)
            .add_extension(x509.ExtendedKeyUsage(
                [ExtendedKeyUsageOID.CLIENT_AUTH, ExtendedKeyUsageOID.SERVER_AUTH]),
                critical=False)
            .sign(key, hashes.SHA256()))
    with open(cert_path, "wb") as fh:
        fh.write(cert.public_bytes(serialization.Encoding.DER))
    with open(key_path, "wb") as fh:
        fh.write(key.private_bytes(serialization.Encoding.PEM,
                                   serialization.PrivateFormat.PKCS8,
                                   serialization.NoEncryption()))


def short(value, limit=110):
    text = repr(value) if not isinstance(value, str) else value
    return text if len(text) <= limit else text[:limit - 3] + "..."


class Changes:
    def __init__(self):
        self.count = {}
        self.last = {}

    def datachange_notification(self, node, val, data):  # asyncua callback
        key = node.nodeid.Identifier
        self.count[key] = self.count.get(key, 0) + 1
        self.last[key] = val


async def make_client(args, policy=None, cert=None, key=None, user=None, password=None):
    from asyncua import Client, ua
    c = Client(args.url, timeout=TIMEOUT)
    c.application_uri = CLIENT_URI
    c.name = "open62541-qnx host client (python asyncua)"
    if policy is not None:
        await c.set_security(policy, certificate=cert, private_key=key,
                             mode=ua.MessageSecurityMode.SignAndEncrypt)
    if user is not None:
        c.set_user(user)
        c.set_password(password)
    return c


async def try_connect(args, **kw):
    """(client or None, error text)"""
    c = await make_client(args, **kw)
    try:
        await asyncio.wait_for(c.connect(), TIMEOUT * 2)
        return c, ""
    except Exception as e:  # noqa: BLE001  (any refusal counts)
        try:
            await c.disconnect()
        except Exception:  # noqa: BLE001
            pass
        return None, f"{type(e).__name__}: {e}"


async def main_async(args):
    from asyncua import ua
    from asyncua.crypto import security_policies as sp

    user, _, password = args.user.partition(":")

    # ---- endpoints
    c = await make_client(args)
    try:
        eps = await asyncio.wait_for(c.connect_and_get_server_endpoints(), TIMEOUT)
        pairs = sorted({(e.SecurityPolicyUri.split("#")[-1], e.SecurityMode.name) for e in eps})
        report(len(eps) > 0, "endpoints",
               f"{len(eps)} endpoints: " + ", ".join(f"{p}/{m}" for p, m in pairs))
    except Exception as e:  # noqa: BLE001
        report(False, "endpoints", f"{type(e).__name__}: {e}")
        return

    # ---- anonymous must be refused
    c, err = await try_connect(args)
    if c:
        await c.disconnect()
    report(c is None, "anonymous", "anonymous session refused: " + (err or "no, it was accepted"))

    # ---- encrypted, with user name: browse and read
    t0 = time.time()
    c, err = await try_connect(args, policy=sp.SecurityPolicyBasic256Sha256, cert=args.cert,
                               key=args.key, user=user, password=password)
    if not c:
        report(False, "browse_read", "Basic256Sha256 SignAndEncrypt, user name: " + err)
        return
    connect_s = time.time() - t0
    try:
        idx = await c.get_namespace_index(NAMESPACE_URI)
        qnx = await c.nodes.objects.get_child([f"{idx}:QNX"])
        nodes = {}
        lines = []
        for node in await qnx.get_children():
            name = (await node.read_browse_name()).Name
            value = await node.read_value()
            nodes[name] = node
            lines.append(f"{name}={short(value)}")
        ok = {"OS", "Uptime", "CpuLoadAverage", "Processes"} <= set(nodes)
        os_name = await nodes["OS"].read_value() if "OS" in nodes else "?"
        report(ok and os_name == "QNX", "browse_read",
               f"Basic256Sha256 SignAndEncrypt as '{user}' (connect {connect_s:.1f} s): "
               f"{len(nodes)} variables below Objects/QNX")
        for line in lines:
            print("O6Q HOST value " + line, flush=True)

        # ---- subscription
        handler = Changes()
        sub = await c.create_subscription(500, handler)
        watch = [nodes[n] for n in ("Uptime", "CpuLoadAverage", "RamFreeMB", "OpcUaSessions")
                 if n in nodes]
        await sub.subscribe_data_change(watch)
        await asyncio.sleep(args.seconds)
        await sub.delete()
        up = handler.count.get("QNX.Uptime", 0)
        report(up >= args.seconds // 2, "subscription",
               f"{up} changes of Uptime and {handler.count.get('QNX.CpuLoadAverage', 0)} of "
               f"CpuLoadAverage in {args.seconds} s; last Uptime "
               f"{short(handler.last.get('QNX.Uptime'))}, CpuLoadAverage "
               f"{short(handler.last.get('QNX.CpuLoadAverage'))}")
    finally:
        await c.disconnect()

    # ---- the other policies both sides know
    for name in ("SecurityPolicyAes128Sha256RsaOaep", "SecurityPolicyAes256Sha256RsaPss"):
        policy = getattr(sp, name, None)
        test = name.replace("SecurityPolicy", "")
        if policy is None:
            print(f"O6Q SKIP host.{test} this asyncua has no {name}", flush=True)
            continue
        c, err = await try_connect(args, policy=policy, cert=args.cert, key=args.key,
                                   user=user, password=password)
        if c:
            try:
                ns = await c.get_namespace_index(NAMESPACE_URI)
                up = await c.get_node(ua.NodeId("QNX.Uptime", ns)).read_value()
                report(True, test, f"SignAndEncrypt, user name; Uptime {short(up)} s")
            finally:
                await c.disconnect()
        else:
            report(False, test, err)

    # ---- refusals
    c, err = await try_connect(args, policy=sp.SecurityPolicyBasic256Sha256, cert=args.cert,
                               key=args.key, user=user, password=password + "-wrong")
    if c:
        await c.disconnect()
    report(c is None, "wrong_password", "wrong password refused: " + (err or "no, accepted"))

    with tempfile.TemporaryDirectory() as tmp:
        cert2, key2 = os.path.join(tmp, "other.der"), os.path.join(tmp, "other.pem")
        make_cert(cert2, key2, "o6q unknown client")
        c, err = await try_connect(args, policy=sp.SecurityPolicyBasic256Sha256, cert=cert2,
                                   key=key2, user=user, password=password)
        if c:
            await c.disconnect()
        report(c is None, "untrusted_cert",
               "certificate outside the trust directory refused: " + (err or "no, accepted"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("url", nargs="?")
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--user", default="operator:")
    ap.add_argument("--seconds", type=int, default=15)
    ap.add_argument("--make-cert", nargs=2, metavar=("CERT", "KEY"))
    args = ap.parse_args()
    if args.make_cert:
        make_cert(*args.make_cert)
        print(f"certificate {args.make_cert[0]}, key {args.make_cert[1]}")
        return 0
    if not (args.url and args.cert and args.key):
        ap.error("URL, --cert and --key are needed")
    import asyncua
    print(f"O6Q HOST client python {sys.version.split()[0]} asyncua "
          f"{getattr(asyncua, '__version__', '?')} url {args.url}", flush=True)
    asyncio.run(main_async(args))
    print(f"O6Q HOST done failures={failures}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
