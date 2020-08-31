import argparse
import string
import os

parser = argparse.ArgumentParser()
parser.add_argument("playername")
parser.add_argument("option")
parser.add_argument("phas")
parser.add_argument("server")
args = parser.parse_args()
#print(args.playername,args.option)
playername=args.playername
option=args.option
phase=args.phas
server=args.server

if server=="1vs1": f=open("game_stat_1vs1.txt","r")
else: f=open("game_stat.txt","r")
lines=f.readlines()

found=0
for line in lines:
    line_split=str.split(line)
    if line_split[0]==playername:
        found=1
        games=line_split[1]
        goals=line_split[2]
        if server=="1vs1": elo=line_split[3]
        if option=="goals":
            if server=="1vs1":g=open("tmpout2.txt","w")
            else: g=open("tmpout.txt","w")
            for line2 in lines:
                if str.split(line2)[0]==playername:
                    if server=="1vs1": g.write(playername+' '+games+' '+str(int(goals)+1)+' '+str(elo)+'\n')
                    else: g.write(playername+' '+games+' '+str(int(goals)+1)+'\n')
                    continue
                g.write(line2)
            break
        if option=="games":
            if server=="1vs1":g=open("tmpout2.txt","w")
            else: g=open("tmpout.txt","w")
            for line2 in lines:
                if str.split(line2)[0]==playername:
                    if server=="1vs1": g.write(playername+' '+str(round(float(games)+1,2)) + ' '+goals + ' '+str(elo)+'\n') 
                    else: g.write(playername+' '+str(round(float(games)+1,2))+' '+goals+'\n')
                    continue
                g.write(line2)
            break
        if option=="leftgame":
            if server=="1vs1":g=open("tmpout2.txt","w")
            else: g=open("tmpout.txt","w")
            for line2 in lines:
                if str.split(line2)[0]==playername:
                    if server=="1vs1":
                        if float(phase)<0: g.write(playername+' '+str(round(float(games)-1.0*float(phase),2))+' '+goals+' '+str(elo)+'\n')
                        else: g.write(playername+' '+str(round(float(games)-1+float(phase),2))+' '+goals+' '+str(elo)+'\n')
                        continue
                    else:
                        if float(phase)<0: g.write(playername+' '+str(round(float(games)-1.0*float(phase),2))+' '+goals+'\n')
                        else: g.write(playername+' '+str(round(float(games)-1+float(phase),2))+' '+goals+'\n')
                        continue
                g.write(line2)
            break
        
if found==0:
    print("New player found")
    if option=="goals":
        if server=="1vs1":g=open("tmpout2.txt","w")
        else: g=open("tmpout.txt","w")
        for line2 in lines:
            g.write(line2)
        if server=="1vs1": g.write(playername+' '+str(0)+' '+str(1)+' '+str(1500)+'\n')
        else: g.write(playername+' '+str(0)+' '+str(1)+'\n')
    if option=="games":
        if server=="1vs1":g=open("tmpout2.txt","w")
        else: g=open("tmpout.txt","w")
        for line2 in lines:
            g.write(line2)
        if server=="1vs1": g.write(playername+' '+str(1)+' '+str(0)+' '+str(1500)+'\n')
        else: g.write(playername+' '+str(1)+' '+str(0)+'\n')
    if option=="leftgame":
        if server=="1vs1":g=open("tmpout2.txt","w")
        else: g=open("tmpout.txt","w")
        for line2 in lines:
            g.write(line2)
        if server=="1vs1": g.write(playername+' '+str(0)+' '+str(0)+' '+str(1500)+'\n')
        else: g.write(playername+' '+str(0)+' '+str(0)+'\n')
g.close()
if server=="1vs1": os.system("mv tmpout2.txt game_stat_1vs1.txt")
else: os.system("mv tmpout.txt game_stat.txt")
