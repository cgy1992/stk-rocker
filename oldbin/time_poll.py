import argparse
import string
import os
f=open("time_poll.txt","r")
lines=f.readlines()

parser = argparse.ArgumentParser()
parser.add_argument("playername")
parser.add_argument("time")
parser.add_argument("ican")
args = parser.parse_args()
playername=args.playername
time=args.time
ican=args.ican

poll=[0]*21

if time=="all":
    for i in range(0,len(poll)): poll[i]=1
elif time[0:8]=="weekends":
    if len(time)>8:
        if time[8:10] in ["16","17","18"]:
           tclock=int(time[8:10])
           for i in range(0,2):
               poll[15+3*i+tclock-16]=1
        else:
            for i in range(15,len(poll)): poll[i]=1
    else:
        for i in range(15,len(poll)): poll[i]=1
elif time[0:8]=="weekdays":
    if len(time)>8:
        if time[8:10] in ["16","17","18"]:
           tclock=int(time[8:10])
           for i in range(0,5):
               poll[3*i+tclock-16]=1
        else:
            for i in range(0,15): poll[i]=1
    else:
        for i in range(0,15): poll[i]=1
elif time[0:2] in ["mo","tu","we","th","fr","sa","su"]:
    if time[0:2]=="mo":i=0
    if time[0:2]=="tu":i=1
    if time[0:2]=="we":i=2
    if time[0:2]=="th":i=3
    if time[0:2]=="fr":i=4
    if time[0:2]=="sa":i=5
    if time[0:2]=="su":i=6
    if len(time)>2:
        if time[2:4] in ["16","17","18"]:
           tclock=int(time[2:4])
           poll[3*i+tclock-16]=1
        else:
            for i in range(3*i,3*(i+1)): poll[i]=1
    else:
        for i in range(3*i,3*(i+1)): poll[i]=1
elif time in ["16","17","18"]:
    tclock=int(time)
    for i in range(0,21):
        if (i-(tclock-16))%3==0: poll[i]=1

print(poll)

found=0
g=open("tmpouttip.txt","w")
for line in lines:
    line_split=str.split(line)
    if line_split[0]==playername:
        old_poll=[0]*21
        for i in range(0,21):
            old_poll[i]=int(line_split[i+1])
        print(old_poll)
        g.write(playername)
        for i in range(0,21):
            if poll[i]==1:
                if ican=="ican": poll[i]=1
                elif ican=="icant": poll[i]=0
                g.write(' '+str(poll[i]))
            else: g.write(' '+str(old_poll[i]))
        g.write("\n")
        found=1
        continue
    g.write(line)
        #Player Mo16 Mo17 Mo18 Tu16 Tu17 Tu18 We16 We17 We18 Th16 Th17 Th18 Fr16 Fr17 Fr18 Fr16 Fr17 Fr18 Sa16 Sa17 Sa18 Su16 Su17 Su18
        
if found==0:
    print("New player found")
    g.write(playername)
    for i in range(0,21):
        g.write(' '+str(poll[i]))
    g.write("\n")

g.close()
os.system("mv tmpouttip.txt time_poll.txt")

os.systen("python3 time_poll_online.txt")

