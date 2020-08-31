import argparse
import string
import os
parser = argparse.ArgumentParser()
parser.add_argument("playername")
parser.add_argument("team")
args = parser.parse_args()
playername=args.playername
team=args.team
f=open("supertournament_currentplayers.txt","r")
lines=f.readlines()
do=1
for line in lines:
    player=str.split(line)[0]
    if player=="Player": continue
    if player==playername: do=0;break
if(do):
    f=open("supertournament_currentplayers.txt","a")
    f.write(playername+" 0 "+str(team)+"\n")
    f.close()
