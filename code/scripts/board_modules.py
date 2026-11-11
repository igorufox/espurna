# Bench tool for the module loader of a board over its web server (digest login, the password from
# firmware_esp_net.load_password(), never printed). Terminal commands (MODULES, MODULES.ENABLE <name>, HEAP...) go through
# firmware_esp_net.py:  py -3 firmware_esp_net.py --host <ip> cmd "MODULES"
#
#   py -3 code/scripts/board_modules.py <host> status                 GET /modules: the head, the arena, every module
#   py -3 code/scripts/board_modules.py <host> upload <file> [query]  POST /modules/upload (a .module or the modules file)
#   py -3 code/scripts/board_modules.py <host> remove <name>          POST /modules/remove
#   py -3 code/scripts/board_modules.py <host> restart                POST /modules/restart
#   py -3 code/scripts/board_modules.py <host> sbupdate <url>         ESP32: POST /safeboot/update, the safeboot fetches
#                                                                     the image (merged from 0x0 or app0) from <url>
#   py -3 code/scripts/board_modules.py <host> get <uri>              any GET with the login
#   py -3 code/scripts/board_modules.py <host> off                    POST /modules/off: restart without module code
#   py -3 code/scripts/board_modules.py <host> apply <file> <a,b,c>   the Apply of the MODULES panel: off, wait, POST
#                                                                     /modules/apply (names, then the modules file of
#                                                                     the .ota or the modules file itself), wait, status

import json
import os
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import firmware_esp_net  # noqa: E402


def post(dev, uri, body, ctype, timeout=120):
    c = dev._conn(timeout)
    headers = {"Content-Type": ctype, "Content-Length": str(len(body))}
    auth = dev.auth_header("POST", uri)
    if auth:
        headers["Authorization"] = auth
    c.request("POST", uri, body=body, headers=headers)
    r = c.getresponse()
    text = r.read().decode(errors="replace")
    c.close()
    return r.status, text


def form(dev, uri, fields):
    return post(dev, uri, urllib.parse.urlencode(fields).encode(), "application/x-www-form-urlencoded")


def status(dev):
    st, body = dev.get("/modules", auth=True)
    print(st)
    try:
        j = json.loads(body)
    except ValueError:
        print(body[:500].decode(errors="replace"))
        return
    modules = j.pop("modules", [])
    j.pop("list", None)
    print(json.dumps(j))
    for m in modules:
        print(" ", m)


def upload(dev, path, query):
    data = open(path, "rb").read()
    boundary = "----board" + os.urandom(6).hex()
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="module"; filename="{os.path.basename(path)}"\r\n'
            f'Content-Type: application/octet-stream\r\n\r\n').encode() + data + f"\r\n--{boundary}--\r\n".encode()
    t = time.time()
    st, text = post(dev, "/modules/upload" + query, body, "multipart/form-data; boundary=" + boundary)
    print(st, text, "%.1fs" % (time.time() - t))


def wait_back(dev, timeout=120):
    """the board answers /modules again after a restart"""
    time.sleep(5)
    start = time.time()
    while (time.time() - start) < timeout:
        try:
            st, _ = dev.get("/modules", auth=True)
            if st == 200:
                return time.time() - start + 5
        except OSError:
            pass
        time.sleep(2)
    raise RuntimeError("the board did not come back")


def modules_file(path):
    data = open(path, "rb").read()
    if data[:4] == b"EOT1":
        import module_ota
        parts = [part for kind, _, part in module_ota.read(data) if kind == module_ota.KIND_MODULES]
        return parts[0]
    return data


def apply(dev, path, names):
    data = modules_file(path)
    print(*form(dev, "/modules/off", {}))
    print("back after %.0fs" % wait_back(dev))
    boundary = "----board" + os.urandom(6).hex()
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="names"\r\n\r\n{names}\r\n'
            f'--{boundary}\r\nContent-Disposition: form-data; name="modules"; filename="modules.bin"\r\n'
            f'Content-Type: application/octet-stream\r\n\r\n').encode() + data + f"\r\n--{boundary}--\r\n".encode()
    t = time.time()
    st, text = post(dev, "/modules/apply", body, "multipart/form-data; boundary=" + boundary, timeout=300)
    print(st, text, "%.1fs" % (time.time() - t))
    if st == 200:
        print("back after %.0fs" % wait_back(dev))
    status(dev)


def main(argv):
    if len(argv) < 2:
        print("usage: board_modules.py <host> status | upload <file> [query] | remove <name> | restart | "
              "sbupdate <url> | get <uri>")
        return 2
    dev = firmware_esp_net.Device(argv[0])
    cmd, args = argv[1], argv[2:]
    if cmd == "status":
        status(dev)
    elif cmd == "upload":
        upload(dev, args[0], args[1] if len(args) > 1 else "")
    elif cmd == "remove":
        print(*form(dev, "/modules/remove", {"name": args[0]}))
    elif cmd == "restart":
        print(*form(dev, "/modules/restart", {}))
    elif cmd == "sbupdate":
        print(*form(dev, "/safeboot/update", {"url": args[0]}))
    elif cmd == "off":
        print(*form(dev, "/modules/off", {}))
    elif cmd == "apply":
        apply(dev, args[0], args[1] if len(args) > 1 else "")
    elif cmd == "get":
        st, body = dev.get(args[0], auth=True)
        print(st, len(body), body[:300])
    else:
        print("unknown command", cmd)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
