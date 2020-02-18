drop table if exists num_of_loops;
CREATE TABLE num_of_loops (num int, aux int);
INSERT into num_of_loops (num, aux) select trunc(random() * 120 + 1) as num, aux from generate_series(1,2000) as aux;