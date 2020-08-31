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
f2=open("tmp_super.txt","w")
found=0
for line in lines:
    if str.split(line)[0]==playername:
        goals=int(str.split(line)[1])
        f2.write(playername+' '+str(goals+1)+' '+team+"\n")
        found=1
        continue
    f2.write(line)
if (found==0):
    f2.write(playername+' '+str(1)+' '+team+"\n")
f2.close()
os.system("mv tmp_super.txt supertournament_currentplayers.txt")
