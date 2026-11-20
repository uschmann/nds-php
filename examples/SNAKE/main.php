<?php
// SNAKE - FRISS DIE AEPFEL, BEISS DICH NICHT SELBST
// STEUERUNG: STEUERKREUZ ODER W/A/S/D
cls(); gcls(); sproff();
rect(0, 8, 255, 15, 5, 1); rect(0, 184, 255, 191, 5, 1);
rect(0, 8, 7, 191, 5, 1); rect(248, 8, 255, 191, 5, 1);
// REKORD AUS DER DATEI SNAKE.JSON (IM PROGRAMMORDNER)
$best = 0;
if (file_exists("snake.json")) $best = json_decode(file_get_contents("snake.json"))["best"] ?? 0;
locate(0, 0); echo "SNAKE      PUNKTE: 0"; locate(24, 0); echo "HI: $best";
$grid = [];   // BELEGTE FELDER: $grid[x][y]
$body = [];   // KOERPER ALS [x, y], KOPF AM ENDE
$len = 3; $nx = 1; $ny = 0; $dx = 1; $dy = 0;
$points = 0; $speed = 8;
for ($i = 0; $i < 3; $i++) {
  $body[] = [10 + $i, 12];
  $grid[10 + $i][12] = true;
  stamp(4, (10 + $i) * 8, 96);
}
sprite(1, 3, 96, 96);

// NEUER APFEL AN FREIER STELLE
function new_apple() {
  global $grid, $fx, $fy;
  do {
    $fx = rand(1, 30); $fy = rand(2, 22);
  } while (isset($grid[$fx][$fy]));
  sprite(0, 2, $fx * 8, $fy * 8);
}

// EINGABE (KEINE UMKEHR AUF DER STELLE)
function read_input() {
  global $nx, $ny, $dx, $dy;
  $j = joy();
  $k = strtoupper(inkey());
  if (($j & JOY_UP || $k == "W") && $dy == 0) { $nx = 0; $ny = -1; }
  if (($j & JOY_DOWN || $k == "S") && $dy == 0) { $nx = 0; $ny = 1; }
  if (($j & JOY_LEFT || $k == "A") && $dx == 0) { $nx = -1; $ny = 0; }
  if (($j & JOY_RIGHT || $k == "D") && $dx == 0) { $nx = 1; $ny = 0; }
}

new_apple();
while (true) {
  for ($w = 0; $w < $speed; $w++) { read_input(); pause(1); }
  $dx = $nx; $dy = $ny;
  $head = $body[count($body) - 1];
  $x = $head[0] + $dx; $y = $head[1] + $dy;
  if ($x < 1 || $x > 30 || $y < 2 || $y > 22 || isset($grid[$x][$y])) break;
  $body[] = [$x, $y];
  $grid[$x][$y] = true;
  stamp(4, $x * 8, $y * 8);
  sprite(1, 3, $x * 8, $y * 8);
  if ($x == $fx && $y == $fy) {
    // APFEL GEFRESSEN
    $points += 10; $len++; sound(0);
    locate(19, 0); echo $points;
    if ($speed > 3 && $len % 4 == 0) $speed--;
    new_apple();
  }
  if (count($body) > $len) {
    $tail = array_shift($body);
    rect($tail[0] * 8, $tail[1] * 8, $tail[0] * 8 + 7, $tail[1] * 8 + 7, -1, 1);
    unset($grid[$tail[0]][$tail[1]]);
  }
}
// GAME OVER
sfx("explosion"); sound(1);
locate(11, 10); echo "GAME OVER";
locate(9, 12); echo "PUNKTE: $points";
if ($points > $best) {
  file_put_contents("snake.json", json_encode(["best" => $points]));
  locate(8, 14); echo "NEUER REKORD!";
  sfx("powerup");
}
#SPR 2 ....5......5.....AA2AA..AAAAAAA.AAAAAAA.AAAAAAA..AAAAA....A.A...
#SPR 3 .DDDDDD.DDDDDDDDD1DDDD1DD0DDDD0DDDDDDDDDDDDDDDDDDD2222DD.DDDDDD.
#SPR 4 555555555DDDDDD55DDDDDD55DD55DD55DD55DD55DDDDDD55DDDDDD555555555
#SND 0 2 240 76:1 84:1
#SND 1 1 160 67:2 64:2 60:2 55:4
