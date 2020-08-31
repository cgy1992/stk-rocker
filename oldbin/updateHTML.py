import os
import string
import time

f=open("tmp.html","r")
if os.path.isfile("new_html"): time.sleep(2)

g=open("new_html","w")
st=open("game_stat.txt","r")
lst=st.readlines()
lines=f.readlines()
for i in range(0,49):
    g.write(lines[i])
for i in range(1,len(lst)):
    split=str.split(lst[i])
    g.write("<tr>\n")
    g.write("<td>"+split[0]+"</td>\n")
    g.write("<td>"+str(split[1])+"</td>\n")
    g.write("<td>"+str(split[2])+"\n")
    if i!= (len(lst)-1): g.write("</td></tr>\n")
g.write("</td></tr></tbody><tfoot></tfoot></table>\n")
g.close()

os.system("mv new_html /var/www/html/3vs3server/index.html")
