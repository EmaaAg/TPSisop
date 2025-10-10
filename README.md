# Ejercicio 01
g++ -std=gnu++17 app.cpp  -o app
./app 2 20 datos.csv

# Ejercicio 02 
<h2> Server </h2> 
<p> g++ -std=gnu++17 server.cpp -o server</p>
<p>./server 8080 datos.csv 5</p>

<h2> Client</h2>
<p> g++ -std=gnu++17 client.cpp -o client</p>
<p> ./client 127.0.0.1 8080 </p>

<p>QUERY Cordoba</p>
<p>BEGIN_TRANSACTION</p>
<p>ADD 5,Pedro,35,Mendoza,Gen3</p>
<p>MODIFY 1 1,Ana,26,Buenos Aires,Gen1_changed</p>
<p>DELETE 2</p>
<p>COMMIT_TRANSACTION</p>
