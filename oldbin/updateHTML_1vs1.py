import os
import string
import time

f=open("tmp_1vs1.html","r")
if os.path.isfile("new_html_1vs1"): time.sleep(2)

g=open("new_html_1vs1","w")
st=open("game_stat_1vs1.txt","r")
lst=st.readlines()
lines=f.readlines()
for i in range(0,50):
    g.write(lines[i])
for i in range(1,len(lst)):
    split=str.split(lst[i])
    g.write("<tr>\n")
    g.write("<td>"+split[0]+"</td>\n")
    g.write("<td>"+str(split[1])+"</td>\n")
    g.write("<td>"+str(split[2])+"</td>\n")
    g.write("<td>"+str(split[3])+"\n")
    if i!= (len(lst)-1): g.write("</td></tr>\n")
g.write("</td></tr></tbody><tfoot></tfoot></table>\n")
g.close()

os.system("mv new_html_1vs1 /var/www/html/1vs1server/index.html")
