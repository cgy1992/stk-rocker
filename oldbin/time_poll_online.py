import os
import string
f=open("time_poll.txt","r")
g=open("stk-supertournament.html","r")
g2=open("new_super_html","w")
lines=g.readlines()
timepoll=f.readlines()
i=0
while i<len(lines):
    g2.write(lines[i])
    if lines[i]=="<h2 class=\"sectionedit15\" id=\"time_poll\">6. Time Poll</h2>\n":
        for j in range(1,13):
            g2.write(lines[i+j])
        g2.write("		<td class=\"col0\">")
        time_poll=str.split(timepoll[1])
        g2.write(time_poll[0]+" </td>")
        for j in range(1,22):
            if time_poll[j]=="0":g2.write("<td class=\"col1 centeralign\">  </td>")
            else: g2.write("<td class=\"col1 centeralign\">  y  </td>")
        g2.write("\n")
        i+=11+len(timepoll)
    i+=1
os.system("mv new_super_html stk-supertournament.html")
