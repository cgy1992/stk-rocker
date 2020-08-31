import argparse
import string
import os
import time

parser = argparse.ArgumentParser()
parser.add_argument("server")
args = parser.parse_args()
server=args.server

if server=="1vs1_2":
    if (os.path.isfile("tmpgst_3.txt") or os.path.isfile("tmpgst.txt") or os.path.isfile("warn.txt") ): time.sleep(2)
    f2=open("tmpgst_2.txt","w")
    f=open("current_1vs1_2_players.txt","r")
elif server=="1vs1_3":
    if (os.path.isfile("tmpgst_2.txt") or os.path.isfile("tmpgst.txt") or os.path.isfile("warn.txt")): time.sleep(2)
    f2=open("tmpgst_3.txt","w")
    f=open("current_1vs1_3_players.txt","r")
else:
    if (os.path.isfile("tmpgst_2.txt") or os.path.isfile("tmpgst_3.txt") or os.path.isfile("warn.txt")): time.sleep(2)
    f2=open("tmpgst.txt","w")
    f=open("current_1vs1_players.txt","r")

lines=f.readlines()
elo1=float(str.split(lines[1])[2])
goals1=int(str.split(lines[1])[1])
name1=str.split(lines[1])[0]
elo2=float(str.split(lines[2])[2])
goals2=int(str.split(lines[2])[1])
name2=str.split(lines[2])[0]
f.close()

cheat=0
if name1==name2: cheat=1

if cheat==0:
    ea_1=(18* 1/(1+10**((elo2-elo1)/400)) -9)
    ea_2=(18* 1/(1+10**((elo1-elo2)/400)) -9)

    elo_new_1 = elo1 + 2*((goals1-goals2) - ea_1)
    elo_new_2 = elo2 + 2*((goals2-goals1) - ea_2)

    f=open("game_stat_1vs1.txt","r")
    lines=f.readlines()

    found1=0
    found2=0
    for line in lines:
        if str.split(line)[0] == name1:
            f2.write(name1+' '+str(int(str.split(line)[1])+1) + ' '+str(int(str.split(line)[2])+goals1) +' '+str(elo_new_1)+'\n')
            found1=1
            continue
        if str.split(line)[0] == name2:
            found2=1
            f2.write(name2+' '+str(int(str.split(line)[1])+1) + ' '+str(int(str.split(line)[2])+goals2)+' '+str(elo_new_2)+'\n')
            continue
        f2.write(line)
    
    if(found1==0): f2.write(name1+' '+'1'+' '+str(goals1) +' '+str(elo_new_1)+'\n')
    if(found2==0): f2.write(name2+' '+'1'+' '+str(goals2) +' '+str(elo_new_2)+'\n')

    f2.close()
    if server=="1vs1_2":
        os.system("mv tmpgst_2.txt game_stat_1vs1.txt")
        f.close()
        f=open("current_1vs1_2_players.txt","w")
        f.close()
    elif server=="1vs1_3":
        os.system("mv tmpgst_3.txt game_stat_1vs1.txt")
        f.close()
        f=open("current_1vs1_3_players.txt","w")
        f.close()
    else:
        os.system("mv tmpgst.txt game_stat_1vs1.txt")
        f.close()
        f=open("current_1vs1_players.txt","w")
        f.close()
