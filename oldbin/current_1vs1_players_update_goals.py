import argparse
import string
import os
parser = argparse.ArgumentParser()
parser.add_argument("playername")
parser.add_argument("server")
args = parser.parse_args()
playername=args.playername
server=args.server

if server=="1vs1_2":
    f=open("current_1vs1_2_players.txt","r")
    f2=open("tmp_1vs1_2.txt","w")
elif server=="1vs1_3":
    f=open("current_1vs1_3_players.txt","r")
    f2=open("tmp_1vs1_3.txt","w")
else:
    f=open("current_1vs1_players.txt","r")
    f2=open("tmp.txt","w")
lines=f.readlines()
for line in lines:
    if str.split(line)[0]==playername:
        goals=int(str.split(line)[1])
        elo=str.split(line)[2]
        f2.write(playername+' '+str(goals+1)+' '+elo+"\n")
        continue
    f2.write(line)
f2.close()
if server=="1vs1_2": os.system("mv tmp_1vs1_2.txt current_1vs1_2_players.txt")
elif server=="1vs1_3": os.system("mv tmp_1vs1_3.txt current_1vs1_3_players.txt")
else: os.system("mv tmp.txt current_1vs1_players.txt")
