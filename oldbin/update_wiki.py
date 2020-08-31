import threading
import os
import argparse

def skript2(server):
    if server=="1vs1": os.system("python3 update_wiki_skript_1vs1.py")
    else: os.system("python3 update_wiki_skript.py")

print("hi")
parser = argparse.ArgumentParser()
parser.add_argument("server")
args = parser.parse_args()
server=args.server
threads = []
t = threading.Thread(target=skript2(server))
threads.append(t)
t.daemon = True
t.start()
