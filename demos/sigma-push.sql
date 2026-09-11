CREATE TABLE departments(id INTEGER PRIMARY KEY, name TEXT);
CREATE TABLE courses(code INTEGER PRIMARY KEY, title TEXT, id INTEGER);
INSERT INTO departments VALUES(1, "Engineering");
INSERT INTO departments VALUES(2, "Math");
INSERT INTO courses VALUES(100, "Intro to Programming", 1);
INSERT INTO courses VALUES(200, "Calculus", 2);
INSERT INTO courses VALUES(300, "Advanced Calculus", 2);
.opt "SELECT title FROM courses NATURAL JOIN departments WHERE courses.code > 150;"
SELECT title FROM courses NATURAL JOIN departments WHERE courses.code > 150;
