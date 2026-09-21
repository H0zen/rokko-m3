# Mișcarea server-side: concept, constrângeri, proiect

Documentul acesta definește ce este mișcarea pe care o produce serverul, ce o
constrânge, cine se mișcă, în câte forme, ce acceptă efectiv clientul pe fir și
cum se atribuie mișcarea implicită unei entități. Este scris înainte de orice
linie de cod nouă și înlocuiește motorul existent, nu îl ajustează.

Sursele sunt două, și numai două: binarul clientului 4.3.4 build 15595 și
codecurile de fir din `src/proto/wire/`, care au proveniență declarată și sunt
verificate împotriva capturilor de client real. Structura veche nu este sursă.

---

## 1. Ce înseamnă „mișcare server-side"

Sub același cuvânt stau trei lucruri care nu au nimic în comun în afară de nume,
și confuzia dintre ele este originea majorității complicațiilor din codul vechi.
Le separ de la început, fiindcă fiecare are alt autor al adevărului.

**A. Mișcare cu autor serverul.** Serverul decide unde ajunge o unitate și îi
spune clientului. Ieșirea este `SMSG_MONSTER_MOVE`. Clientul nu are drept de vot:
desenează ce a primit. Aici trăiește motorul pe care îl proiectăm.

**B. Mișcare cu autor clientul.** Jucătorul își mută propriul personaj. Clientul
trimite `CMSG_MOVE_*`, serverul validează, actualizează și retransmite celorlalți.
Serverul nu produce nimic; el arbitrează și rebroadcastează. Această parte există
deja în `src/motion/` — `State`, `Authority`, `TimeBase`, `PendingChanges`,
`Change`, `PacketMatrix`, `Writers` — și **nu este motor de mișcare**. Rămâne.

**C. Mișcare fără autor.** Transporturile și lifturile se mișcă pe trasee
periodice derivate din DBC. Clientul calculează singur poziția din aceleași date.
Serverul nu trimite nimic; are nevoie doar să știe unde se află, ca să poată așeza
pasagerii. Nu este trafic, este o funcție de timp.

Proiectul acesta acoperă **exclusiv A**. Orice tentație de a unifica A cu B
produce exact abstracția care a umflat codul vechi.

---

## 2. Alfabetul de ieșire: un singur mesaj

Tot ce poate serverul să exprime despre mișcarea unei unități încape într-o
structură, `Wire::MonsterMove` (`src/proto/wire/MonsterMoveCodec.h`), verificată
pe 250 de familii de pachete capturate de la client real. Gradele ei de libertate
sunt toate gradele de libertate ale motorului:

| Ce | Cum se exprimă |
|---|---|
| un traseu | polilinie: `Linear` (destinație + offseturi împachetate) sau `Uncompressed` |
| viteza | implicit, prin `duration` peste lungimea poliliniei |
| o parabolă | `kSplineFlagTrajectory` + `verticalAcceleration` + `parabolicStart` |
| o animație pe drum | `kSplineFlagAnimation` + `animationId` + `animationStart` |
| orientarea finală | `MonsterMoveType`: `FacingSpot`, `FacingTarget`, `FacingAngle`, sau niciuna |
| oprire | `MonsterMoveType::Stop` — pachetul se termină acolo |
| cadru de referință | forma `_TRANSPORT` cu guid de vehicul și scaun |

**Consecința de proiectare, și este cea mai importantă din document:** pe fir nu
există „urmărire", „patrulare", „rătăcire" sau „fugă". Există o polilinie și o
durată. Cele cincisprezece „tipuri de mișcare" din codul vechi sunt cincisprezece
moduri de a decide *ce pui în structura asta data viitoare*. Orice concept din
motor care nu se reduce la un câmp de aici trebuie să-și justifice existența.

---

## 3. Constrângerile clientului

Transcrise din binar în sesiunea de analiză, nu presupuse. Fiecare este o regulă
pe care serverul o încalcă tăcut dacă nu o cunoaște.

**C1 — Clientul nu are încredere în durata noastră.** Măsoară singur lungimea
poliliniei pe care a construit-o *el*, împarte la o viteză aleasă de el și
folosește rezultatul. `duration` nu este un orar, este felul în care scriem o
viteză.

**C2 — Există un plafon de viteză.** `max(4 × viteza_moverului, 28)`, sau 50 fix
când flagurile ating masca `0x02000A40`. Peste plafon clientul nu refuză, ci
întinde: unitatea ajunge târziu și nimic nu raportează.

**C3 — Clientul reconstruiește primul punct din poziția declarată în pachet.**
Pe fir, un traseu `Linear` cu mai multe puncte **nu trimite primul punct**: trimite
poziția de start ca trei floats la începutul pachetului, destinația, și offseturile
din mijloc. Clientul compară primul punct pe care l-a obținut cu poziția de start
declarată și, dacă diferă, o inserează în față.

Pragul diferă pe ramuri, și diferența este reală:

| Ramură | Prag de inserare |
|---|---|
| `Uncompressed` (flag `0x00400000`) | distanță² > `0.00077160494` = (1/36 yd)² |
| `Linear` cu un singur punct | aceeași, `0.00077160494` |
| `Linear` cu mai multe puncte | **orice** diferență: `> 0.0` |

Poziția *proprie* a clientului intră în ecuație într-un singur loc, la tipul
`Stop`: acolo compară unde crede el că este unitatea cu poziția din pachet, față
de o valoare de configurare (`dword_DD6180+44`), și numai dacă sunt aproape face
o oprire curată; altfel trece prin construcția completă a traseului.

**C4 — Segmentele de lungime zero îl omoară.** Două puncte consecutive identice
produc o împărțire la zero într-un atan2 în virgulă fixă (`sub_AC3C00`) și
clientul cade cu ERROR #132. Serverul este singurul care poate preveni asta.
Clientul deduplică **numai** traseele ciclice (flag `0x1000`), cu epsilonul
`2.4e-7`, și numai la capătul de închidere a buclei.

**C5 — Traseul împachetat se poate înfășura.** Punctele intermediare ale unui
traseu `Linear` sunt offseturi față de `mijloc(start, destinație)`, cuantizate la
0.25 yd în câmpuri cu semn de 11, 11 și 10 biți: ±256 yd pe X și Y, ±128 pe Z.
Un yard peste, câmpul **nu saturează, ci își schimbă semnul**. Testul este per
punct și față de mijloc, nu pe o cutie de încadrare.

**C6 — Există DOUĂ lungimi, nu sunt egale, și fiecare guvernează altceva.**
Lanțul este dovedit cap la cap:

1. Handler-ul (`sub_5CB460`) însumează **coarde între punctele de control** și
   scrie `durata = coardă / min(plafon, coardă/durata_trimisă) × 1000`.
2. La instalare, obiectul spline își calculează un **tabel de lungimi pe
   segment** prin eșantionare în 20 de pași (`sub_4C96F0`, însumate de
   `sub_4C9880`), și totalul lor devine lungimea obiectului.
3. La fiecare cadru se evaluează cu `t = scurs / durată` (`sub_4C93D0`), iar
   evaluatorul (`sub_4C97C0`) face `distanță = lungime_totală × t` și **parcurge
   tabelul de segmente** ca să găsească segmentul și fracția locală.

Pasul 3 este **parametrizare după lungimea de arc**: viteza este constantă
de-a lungul curbei reale. Dar durata de la pasul 1 a fost calculată din coardă.

**Consecința, exactă:** unitatea parcurge lungimea reală într-un timp derivat din
coardă, deci

```
viteza_efectivă = viteza_cerută × (arc / coardă)
```

Coarda nu poate depăși arcul, deci **un traseu curbat se mișcă întotdeauna mai
repede decât am cerut, niciodată mai încet.** Pentru un traseu drept arcul este
egal cu coarda și eroarea este zero — greșeala este exclusiv a traseelor curbate,
și crește cu curbura: câteva procente la viraje line, peste treizeci la colțuri
strânse.

Serverul are două ieșiri corecte și nicio a treia: fie trimite trasee liniare și
își face singur netezirea în puncte, fie, pentru un traseu curbat, scrie durata
din lungimea eșantionată în **20 de pași pe segment**, adică exact cum o măsoară
clientul. Orice altă eșantionare produce o viteză greșită.

**C6b — Clientul are o resincronizare de fază de până la ±2×.** Obiectul spline
ține două scale de timp (offset 516 și 520, ambele 1.0 la instalare), iar
`sub_A204A0` ajustează a doua ca să aducă faza la o valoare cerută, **limitată la
intervalul [0.5, 2.0]**. În decompilare nu am găsit niciun apelant direct, deci
fie se ajunge la ea printr-un pointer de funcție, fie este folosită doar pentru
transporturi. De reținut că mecanismul există: clientul *poate* accelera sau
încetini un spline ca să prindă o fază, dar nu mai mult de dublu.

**C7 — După trimitere, tăcere.** Nu există pe fir niciun mesaj prin care clientul
să raporteze progresul unui spline. Odată trimis, clientul merge singur până la
capăt. **Aceasta este constrângerea din care iese toată arhitectura.**

**C8 — Un spline refuzat nu oprește unitatea, o teleportează.** Dacă viteza
rezultată este sub `9.5367432e-7`, sau dacă instalarea eșuează (flagurile ating
`0x18000`), clientul nu pornește nicio interpolare: pune unitatea **direct la
destinație**. Un traseu prost formulat nu produce o creatură care stă pe loc, ci
una care sare.

**C9 — Tipul orientării devine un flag de spline.** `FacingSpot` adaugă
`0x04000000`, `FacingTarget` adaugă `0x08000000`, `FacingAngle` adaugă
`0x10000000` peste flagurile trimise.

**C10 — Jucătorul raportează cel puțin la 500 ms.** După orice pachet de stare de
mișcare, clientul își programează următorul `MSG_MOVE_HEARTBEAT` la `acum + 500`
ms (`sub_573470`), și își sub-împarte pasul de simulare ca să nimerească exact
momentul. Când translația se oprește, programează o confirmare la `acum + 125` ms.
Masca care cere heartbeat este `0x0060080F` — înainte, înapoi, pași laterali,
cădere, urcare și coborâre. **Rotirea pe loc nu generează heartbeat.**

Deci vechimea maximă a poziției unui jucător, în absența pierderilor, este
**500 ms plus latența**, și aceea este toleranța de la §"Asimetria cunoașterii".

**C11 — Clientul NU proiectează niciodată polilinia pe teren.** Calea care scrie
poziția unei unități conduse de spline este una singură (`sub_A27380`): evaluează
(`sub_4C9500`), scrie cele trei coordonate și atât. Nu există nicio interogare de
înălțime, de teren sau de coliziune pe traseul acela. Singurele două atingeri ale
lui Z sunt suprapuneri explicite, ambele prin `sub_A20D40`: parabola (flag
`0x2000000`) și căderea liberă (flag `0x40`).

Prin urmare **S1 nu este o preferință de proiectare, este o obligație**: dacă
serverul trimite un Z greșit, unitatea plutește sau intră în pământ, și nimeni nu
o corectează. Geometria este integral a serverului.

**C12 — Orientarea vine gratis din tangentă.** După fiecare evaluare, dacă
`(flaguri & 0x448) == 0` și tangenta orizontală depășește `0.0018490001`,
clientul scrie `orientare = atan2(tangentă.y, tangentă.x)`. Flagul `0x80000`
scade π, adică unitatea merge cu spatele. Înclinarea se scrie din
`asin(tangentă.z)` când unitatea zboară sau plutește.

Deci **serverul nu trebuie să trimită niciodată nimic doar ca să rotească o
unitate care se mișcă.** Orientarea finală (`FacingTarget` și celelalte) contează
numai la capătul splinei.

**C13 — Clientul poate rotunji colțurile singur.** CVar-ul `pathSmoothing`
("NPC will round corners on ground paths", implicit pornit) este citit chiar în
handler: dacă e stins, clientul șterge flagul `0x00100000` din flagurile primite.
Cu flagul pornit, clientul construiește punctele de control fantomă de la capete:
la plecare din direcția în care unitatea privește deja (dacă produsul scalar cu
primul segment este sub `0.7`, adică peste ~45°, folosește direcția segmentului
dublată), iar la sosire prelungește ultimul segment cu până la **2 yarzi**.

Asta este alternativa la a trimite un traseu curbat și a plăti eroarea de la C6:
**traseu liniar plus flagul `0x00100000`**, iar netezirea se face la client.

**C14 — Scala de timp și resincronizarea ciclică.** Durata efectivă nu este
`durata` trimisă, ci `durata × scala(offset 516)`, rotunjită. La fiecare
închidere de ciclu a unui spline ciclic (flag `0x1000`), scala activă devine
factorul de resincronizare (offset 520) și acela se resetează la 1.0. Deci
resincronizarea de fază de la C6b **se aplică ciclu cu ciclu, pe spline ciclice**
— transporturile — și nu poate depăși dublul sau jumătatea vitezei.

**Alte constante și flaguri stabilite pe parcurs:**

| Ce | Valoare |
|---|---|
| gravitația clientului | `19.291105` yd/s² |
| `pathDistTol` (pragul de oprire curată, C3) | CVar, implicit **1 yard** |
| `pathSmoothing` | CVar, implicit **pornit** |
| flag ciclic | `0x00001000` |
| flag rotunjire colțuri | `0x00100000` |
| flag mers cu spatele | `0x00080000` |
| flag traseu neîmpachetat | `0x00400000` |
| flag parabolă | `0x02000000` |
| flag animație | `0x01000000` |
| flag cădere | `0x00000040` |
| spline terminat / în pauză | `0x20` / `0x4000` |
| masca ce suprimă orientarea din tangentă | `0x448` |

**Rămas de verificat:** nimic din lista inițială. Toate trei întrebările deschise
la prima trecere — proiecția pe teren, valoarea lui `pathDistTol` și apelantul
resincronizării — sunt închise mai sus.

---

## 4. Constrângerile serverului

**S1 — Geometria este a noastră.** Clientul desenează linia primită. Dacă trece
printr-un zid, unitatea trece prin zid. Navmesh-ul nu este o optimizare, este
condiția de corectitudine.

**S2 — Pachetul pleacă la toți observatorii.** Costul unei decizii nu este
decizia, ci difuzarea ei. Un traseu lung trimis o dată este mai ieftin decât
același traseu în zece bucăți.

**S3 — Un observator care intră târziu trebuie să vadă același lucru.** Blocul de
mișcare din `SMSG_UPDATE_OBJECT` trebuie să fie derivabil din aceeași sursă ca
pachetul trimis la plecare. Dacă sunt două reprezentări, vor diverge.

**S4 — Eșecul are un singur înțeles.** Un traseu care nu poate fi calculat
înseamnă „încearcă din nou", niciodată „am ajuns" și niciodată „treci mai
departe". Confuzia asta a fost cauza reală a patrulelor care săreau noduri:
o rută eșuată avansa nodul curent ca și cum ar fi fost atins.

---

## 5. Entitățile care se mișcă

Clasificarea utilă nu este pe tipul de obiect, ci pe **cine deține adevărul**.

| Clasă | Cine sunt | Autoritatea | Ce primește motorul |
|---|---|---|---|
| Condusă de server | creaturi, familiari, gardieni, vehicule fără șofer | serverul | tot |
| Condusă de client | jucători | clientul | nimic, cât timp se conduc singuri |
| Confiscată temporar | un jucător în zbor pe taxi, aruncat, fricoșat, tras | serverul, pe durata efectului | traseul, apoi controlul înapoi |
| Cinematică | transporturi, lifturi | nimeni, este o funcție de timp | nimic |

Confiscarea este singurul loc unde A și B se ating, și se atinge printr-un
protocol explicit (preluarea și returnarea controlului), nu prin partajarea unei
structuri de stare.

O entitate **nu este** „ceva care are un MotionMaster". Este ceva pentru care
serverul poate produce un `MonsterMove`. Un totem nu se mișcă; nu are nevoie de
nicio structură.

---

## 6. Formele mișcării: șase, nu cincisprezece

Codul vechi avea cincisprezece clase de comportament. Dacă le clasifici după
*ce decide destinația* și *ce anume obligă serverul să acționeze din nou*, rămân
șase forme. Restul sunt etichete de joc peste aceleași șase.

| Formă | Ce decide destinația | Când trebuie serverul să revină |
|---|---|---|
| **Traseu scris** | o listă cunoscută dinainte (patrulă, taxi) | la un nod care obligă la oprire, sau când se termină bugetul de împachetare |
| **Urmărire** | poziția vie a altei unități (chase, follow) | la prima clipă în care separarea *poate* depăși toleranța |
| **Un singur punct** | un punct fix (point, home, aterizare, alergare spre aliat) | la sosire |
| **Împrăștiere** | o extragere aleatoare într-o regiune (wander, confuz) | la sosire, plus odihnă |
| **Respingere** | direcția opusă unei surse (frică) | la sosire, sau când sursa s-a mutat destul |
| **Balistică** | parametri fizici, nu un traseu (salt, aruncare, cădere) | la aterizare |

Fiecare formă are aceeași semnătură: *dă-mi un `MonsterMove` și spune-mi când să
te întreb din nou*. Diferența dintre o patrulă și o urmărire este ce calculează
în interior, nu forma răspunsului.

Etichetele de joc — `Patrol`, `Chase`, `Fear` — rămân, dar numai ca să știm ce
raportăm scripturilor și AI-ului. Ele nu sunt tipuri în motor.

---

## 7. Prioritate: două concepte, nu douăzeci și unu

Vechiul model avea șapte straturi × trei politici. Ce trebuie de fapt să fie
adevărat este mult mai puțin:

**Întreruperi, care se stivuiesc.** O frică întrerupe o urmărire; când frica se
termină, urmărirea continuă fără ca nimeni să o fi salvat undeva. O stivă mică,
cu adâncime mărginită de numărul de cauze care pot întrerupe simultan.

**Interdicții, care se numără.** Rădăcinare, amețire, posedare, moarte: cât timp
o interdicție este ținută de cel puțin o sursă, unitatea nu poate fi mutată de
nimeni. Este o mulțime de surse per motiv, nu un strat. Partea aceasta există
deja și funcționează (`src/motion/Mobility.h`) — se păstrează.

O înlocuire (o patrulă nouă peste una veche) nu are nevoie de politică: pui
altceva în locul a ce era, iar ce era dispare. Trei politici distincte descriau
trei consecințe ale aceleiași operații.

---

## 8. Mecanismul: plan și ceas, nu tick

Din **C7** rezultă direct arhitectura. Odată trimis pachetul, clientul merge
singur. Un server care vizitează fiecare creatură la fiecare tick de lume
simulează ceva ce se întâmplă deja în altă parte — și îl simulează greșit,
fiindcă **C1** spune că clientul recalculează durata oricum.

**Un mover nu este ticăit. Spune când are următoarea treabă.**

Harta ține o singură coadă de priorități `(moment, mover)`. La fiecare tick se
scot cei scadenți. O creatură care merge un traseu de cinci secunde nu apare în
coadă timp de cinci secunde. Una care stă la un waypoint nu apare până nu-i
expiră pauza. Una inactivă nu apare deloc.

Ce trezește devreme un mover este un **eveniment** — a fost atacat, a fost
fricoșat, ținta lui s-a mutat — iar un eveniment este un apel, nu o interogare.

### Urmărirea fără interogare

Partea nouă și singura netrivială. O urmărire nu își interoghează ținta. Ea
calculează o **margine** și doarme până la ea.

Mărimea care contează nu este separarea dintre cele două unități, ci **cât a
derivat ținta față de punctul spre care am planificat să merg**. Urmăritorul se
îndreaptă spre un punct fix `E`, ales astfel încât `|E − P|` să fie acceptabil în
momentul planificării. Viteza urmăritorului nu intră în calcul: ea îl duce spre
`E` indiferent de ce face ținta. Ce invalidează planul este mișcarea țintei față
de `E`, mărginită de viteza ei maximă.

Al doilea lucru care contează este că poziția unui jucător este **învechită**:
serverul o știe ca la momentul `t_P` al ultimului raport. Incertitudinea despre
unde este acum și cea despre unde va ajunge sunt aceeași mărime, `v_P × (t − t_P)`,
deci se tratează ca una singură și se măsoară de la raport, nu de la „acum":

```
t_trezire = t_P + toleranță / v_max(P)
```

Este o margine inferioară sigură: nu poate rata evenimentul, oricum s-ar mișca
ținta.

**Consecința care justifică întreaga schemă: ținta care se mișcă nu trebuie să
notifice pe nimeni.** Niciun urmăritor nu poate fi greșit înainte de ora lui de
trezire, prin construcție. Nu există liste de observatori și nu există propagare
„ținta s-a mișcat". Pachetul de mișcare al jucătorului actualizează o poziție și
atât.

Ce trebuie totuși să invalideze marginea este o listă scurtă și închisă, fiindcă
sunt evenimente pe care serverul le autorizează el însuși și care încalcă
ipoteza: **teleportare, schimbare de viteză, moarte sau dispariție, schimbare de
hartă**. Patru cazuri, toate cu autor cunoscut.

Când ținta este tot o creatură condusă de server, **serverul îi cunoaște planul**
și poate calcula momentul exact în loc de margine.

### Asimetria cunoașterii, și ce costă ea

Cele două direcții nu sunt simetrice, și asta decide unde se pune toleranța.

Despre o creatură condusă de server, serverul știe **exact** unde este, la
milisecundă: are planul, iar poziția este o funcție de timp. Zero incertitudine.

Despre un jucător știe doar ultima poziție raportată, cu o vechime cunoscută.
Toată incertitudinea din sistem vine de aici.

Prin urmare orice verificare între un jucător și o creatură în mișcare — rază de
lovire, rază de vrajă, linie de vedere — are o singură sursă de eroare, jucătorul,
și toleranța se dimensionează după `v_max(P) × vechimea raportului`. Nu se adaugă
nimic pentru creatură.

**Dar poziția exactă a creaturii este exactă numai dacă clientul o calculează la
fel.** Aici intră **C1** și **C2**: dacă durata trimisă cere o viteză peste
plafonul clientului, clientul întinde traseul, creatura lui rămâne în urma
creaturii noastre, iar verificarea de rază a jucătorului nu mai este de acord cu
a serverului. Simptomul pe care îl vede jucătorul este „lovesc și îmi spune că e
prea departe".

Deci verificarea plafonului **nu este un diagnostic, este o condiție de
corectitudine a funcției de poziție**. Un traseu care depășește plafonul nu se
trimite.

### Traversarea celulelor fără interogare

Aceeași idee: dintr-un plan se poate calcula analitic la ce momente traseul taie
granițele de celulă. Se programează ca evenimente. Nu mai există comparație de
celulă per tick pentru fiecare unitate.

---

## 9. Poziția este o funcție, nu o stare

O unitate care se mișcă nu are o poziție stocată. Are un plan: `(puncte, viteză,
moment de start)`. `PozițiaLa(t)` este aritmetică pe o sumă cumulativă de
lungimi — o căutare binară și o interpolare.

Placement-ul se scrie **o singură dată**, când planul se termină. Tot ce are
nevoie de poziție — grila, vizibilitatea, vrăjile, raza de agro — cheamă funcția.

Asta elimină prin construcție o clasă întreagă de defecte: poziția scrisă nu mai
poate diverge de traseul în desfășurare, fiindcă nu mai există o poziție scrisă
cât timp traseul rulează. Elimină și eroarea de până la 2.8 yd pe mover care
venea din scrierea periodică a poziției la interval fix.

Costul este un apel de funcție în locul unei citiri de câmp; se amortizează cu un
cache `(t, poziție)` per unitate, invalidat de `t`.

---

## 10. Atribuirea mișcării implicite

Mișcarea implicită nu este „un comportament instalat la baza unei stive". Este
**ce face unitatea când nimic altceva nu este decis** — o proprietate, evaluată
leneș, nu un obiect care ocupă un sertar.

Se derivă o singură dată, la apariție, din datele spawnului: tipul de mișcare din
`creature` / `creature_template`, plus traseul dacă are unul. Se re-derivă numai
când datele spawnului se schimbă (editare de waypoint de către un GM).

Când stiva de întreruperi se golește, motorul întreabă proprietatea, nu un
sertar. Consecința practică: o creatură fără nicio mișcare nu costă nimic și nu
ocupă nimic — nu are intrare în coadă, nu are obiect de comportament, nu are
plan.

---

## 11. Ce se păstrează din ce există

Nu tot ce atinge mișcarea este noroi MaNGOS. Se păstrează, pentru că au
proveniență verificată:

- **`src/proto/wire/`** — codecurile de fir, validate pe capturi de client real.
  `MonsterMoveCodec` este exact alfabetul de ieșire al motorului.
- **`src/motion/`** rămas — protocolul de mișcare al clientului: confirmări,
  autoritatea moverului, baza de timp, schimbările de flaguri. Clasa B, nu motor.
- **`Mobility.h`** — interdicțiile numărate pe surse.
- **Navmesh-ul** (`PathFinder`) — geometria, adică **S1**.

Se șterge tot restul: arbitrul, cele cincisprezece comportamente, driverul,
adaptoarele de cadru și harness-ul de scenarii.

---

## 12. Ce NU construim

Enumerat explicit, ca să nu reapară prin acumulare:

- Nicio simulare server-side a interpolării clientului. **C1** o face greșită
  prin construcție.
- Niciun flux de evenimente și nicio tranzacție în arbitraj. Cine rulează se
  citește; cine vrea să afle dacă s-a schimbat compară înainte și după.
- Niciun strat de „intenții" separat de ceea ce pleacă pe fir. Structura de la
  §2 este intenția.
- Nicio ierarhie de clase pentru cele șase forme dacă o funcție ajunge.
- Niciun vocabular de tip „kernel", „campanie", „politică". Șase forme, o coadă,
  un codec.

---

## 13. Ce forțează constatările din binar

Patru decizii nu mai sunt opțiuni, sunt consecințe.

**Nu trimitem niciodată Catmull-Rom.** Traseul curbat are eroarea arc/coardă de
la C6, obligă serverul să eșantioneze în exact 20 de pași pe segment ca să scrie
o durată corectă, și nu cumpără nimic: clientul rotunjește colțurile singur dacă
îi dăm flagul `0x00100000` (C13). Deci **tot ce pleacă este liniar**, împachetat,
cu flagul de netezire. Asta șterge din start o clasă întreagă de cod și o clasă
întreagă de defecte.

**Fiecare picior trebuie rutat.** Clientul nu proiectează nimic pe teren (C11).
Nu există „trimite drept și se rezolvă": un Z greșit rămâne greșit pe ecran.

**Nimic nu pleacă nevalidat.** Un spline refuzat de client teleportează unitatea
(C8). Deci validarea — lungime, plafon, segmente nenule, raza de împachetare —
se face **înainte** de trimitere, iar rezultatul unui eșec este „încearcă din
nou", niciodată un pachet pe fir.

**Nu trimitem pachete ca să rotim.** Orientarea iese din tangentă la client
(C12). Orientarea explicită contează doar la capătul unei splines.

---

## 14. Planul

Numele sunt cele finale; fiecare piesă face un singur lucru și primele patru se
testează fără server.

### Piesele

| # | Piesă | Ce face | Testabil fără server |
|---|---|---|---|
| 1 | `Path` | polilinie + viteză + moment de start; `PositionAt`, `FacingAt`, `DistanceAt`, `TimeAtDistance`, `EndTime` | da |
| 2 | `MoveWriter` | `Path` + orientare + mers → `Wire::MonsterMove`, cu toate regulile C2–C5, C8, C13 aplicate; **refuză** în loc să producă un pachet care teleportează | da |
| 3 | `Clock` | coada de priorități `(moment, mover)` a hărții | da |
| 4 | `Movement` | per unitate: ce rulează, `Path`-ul în zbor, momentul următoarei treziri | da |
| 5 | cele șase forme | fiecare o funcție: dă un `Path`, spune când să fie întrebată din nou | da, cu o `World` falsă |
| 6 | legarea | `World` peste hartă și navmesh; atribuirea implicită la apariție | nu |

### Ordinea

**Pasul 0 — golirea.** Se șterge motorul: arbitrul, cele cincisprezece
comportamente, driverul, adaptoarele de cadru, harness-ul. `MotionMaster` rămâne
ca fațadă inertă, ca să nu se piardă cele două mii de locuri care *cer* mișcare —
ele sunt specificația, nu implementarea. Rezultat: serverul construiește, rulează,
și nimic nu se mișcă. Asta este starea cerută explicit.

**Pasul 1 — `Path` și `Clock`.** Aritmetică pură, teste proprii. Nimic legat.

**Pasul 2 — `MoveWriter`.** Peste `MonsterMoveCodec`, care are deja proveniență.
Testele includ regresii pe fiecare regulă de client: un traseu cu segment nul
este refuzat, unul care depășește raza de împachetare este tăiat, unul peste
plafon este refuzat, unul prea scurt nu este re-cronometrat.

**Pasul 3 — `Movement`.** Stiva de întreruperi, `Path`-ul în zbor, momentul de
trezire. Interdicțiile se iau din `Mobility.h`, care există.

**Pasul 4 — prima formă: un singur punct.** Cea mai simplă și cea care acoperă
`MovePoint`, adică 445 din cele 907 apeluri din scripturi. Odată ce merge, o bună
parte din joc se mișcă din nou.

**Pasul 5 — traseu scris.** Patrula. Aici se aplică regula S4: o rută eșuată
înseamnă reîncercare, niciodată avans de nod.

**Pasul 6 — împrăștiere, urmărire, respingere, balistică.** În ordinea asta:
împrăștierea e cea mai simplă, urmărirea aduce marginea analitică de la §8,
respingerea o refolosește, balistica nu are traseu, ci parametri.

**Pasul 7 — atribuirea implicită** și legarea la apariția creaturilor.

**Pasul 8 — ștergerea simulării de spline** din `src/game/movement/`. `MoveSpline`
și `spline.impl.h` re-simulează interpolarea clientului ca să afle unde este
unitatea; `Path::PositionAt` răspunde exact la aceeași întrebare, și o face cu
aritmetica pe care o face și clientul. Nu mai au pentru ce să existe.

### Ce măsurăm ca să știm că merge

Trei numere, nu impresii:

1. **Treziri pe secundă** față de vizitele pe tick ale motorului vechi. Așteptarea
   este două ordine de mărime.
2. **Pachete `MonsterMove` pe minut** pentru aceeași scenă. Trebuie să scadă:
   nicio rotire, trasee lungi trimise o dată.
3. **Refuzuri de validare**, pe categorii. Trebuie să fie zero în joc normal;
   orice refuz recurent este un defect al formei care l-a produs, nu al scriitorului.
