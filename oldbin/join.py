import argparse
import string
import os
f=open("tournament_players.txt","r")
lines=f.readlines()

parser = argparse.ArgumentParser()
parser.add_argument("playername")
args = parser.parse_args()
playername=args.playername

found=0
for line in lines:
    line_split=str.split(line)
    if line_split[0]==playername:
        found=1;
        break
        
if found==0:
    print("New player found")
    g=open("tmpouttp.txt","w")
    for line2 in lines:
        g.write(line2)
    g.write(playername +'\n')

g.close()
os.system("mv tmpouttp.txt tournament_players.txt")
