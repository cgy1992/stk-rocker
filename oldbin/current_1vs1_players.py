import argparse
import string
import os
parser = argparse.ArgumentParser()
parser.add_argument("playername")
parser.add_argument("server")
args = parser.parse_args()
playername=args.playername
server=args.server
elofile=open("game_stat_1vs1.txt","r")
elolines=elofile.readlines()
elo=0
for line in elolines:
    if str.split(line)[0]==playername:
        elo=float(str.split(line)[3])
        break
if elo==0: elo=1500.0

if server=="1vs1_2":
    f=open("current_1vs1_2_players.txt","r")
elif server=="1vs1_3":
    f=open("current_1vs1_3_players.txt","r")
else: f=open("current_1vs1_players.txt","r")
lines=f.readlines()
if len(lines)!=2:
    if len(lines)==3:
        line2=lines[2]
        line1=lines[1]
        test=str.split(line1)
        test2=str.split(line2)
        if len(test)==4 and len(test2)==4:
            if server=="1vs1_2":
                f=open("current_1vs1_2_players.txt","w")
            elif server=="1vs1_3":
                f=open("current_1vs1_3_players.txt","w")
            else: f=open("current_1vs1_players.txt","w")
            f.write("Player Goals Elo\n")
            f.write(playername+" 0 "+str(elo)+"\n")
        elif len(test)==4:
            if server=="1vs1_2":
                f=open("current_1vs1_2_players.txt","w")
            elif server=="1vs1_3":
                f=open("current_1vs1_3_players.txt","w")
            else: f=open("current_1vs1_players.txt","w")
            f.write("Player Goals Elo\n")
            f.write(line1)
            f.write(playername+" 0 "+str(elo)+" n \n")
            f.close()
        else:
            if server=="1vs1_2":
                f=open("current_1vs1_2_players.txt","w")
            elif server=="1vs1_3":
                f=open("current_1vs1_3_players.txt","w")
            else: f=open("current_1vs1_players.txt","w")
            f.write("Player Goals Elo\n")
            f.write(playername+" 0 "+str(elo)+" n \n")
            f.write(line2)
            f.close()
    else:
        f.close()
        if server=="1vs1_2":
            f=open("current_1vs1_2_players.txt","w")
        elif server=="1vs1_3":
            f=open("current_1vs1_3_players.txt","w")
        else: f=open("current_1vs1_players.txt","w")
        f.write("Player Goals Elo\n")
        f.write(playername+" 0 "+str(elo)+"\n")
        f.close()
else:
    if server=="1vs1_2":
        f=open("current_1vs1_2_players.txt","a")
    elif server=="1vs1_3":
        f=open("current_1vs1_3_players.txt","a")
    else: f=open("current_1vs1_players.txt","a")
    f.write(playername+" 0 "+str(elo)+"\n")
    f.close()
