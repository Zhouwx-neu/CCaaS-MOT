CREATE schema FVT_COMPRESS_QWER;
set search_path to FVT_COMPRESS_QWER;
-- section 1: test from delete.sql
create table delete_test_list (
    id int,
    a int,
    b text
) partition by list(a)
(
partition delete_test_hash_p1 values(1,2,3,4,5,6,7,8,9,10),
partition delete_test_hash_p2 values(11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50),
partition delete_test_hash_p3 values(51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100));
create  index  delete_test_list_index_local1  on delete_test_list  (a)  local
(
    partition delete_test_list_p1_index_local tablespace PG_DEFAULT,
    partition delete_test_list_p2_index_local tablespace PG_DEFAULT,
    partition delete_test_list_p3_index_local tablespace PG_DEFAULT
);

INSERT INTO delete_test_list (a) VALUES (10);
INSERT INTO delete_test_list (a, b) VALUES (50, repeat('x', 10000));
INSERT INTO delete_test_list (a) VALUES (100);

SELECT id, a, char_length(b) FROM delete_test_list order by 1, 2, 3;

-- Pseudo Constant Quals
DELETE FROM delete_test_list where null;

-- allow an alias to be specified for DELETE's target table
DELETE FROM delete_test_list AS dt WHERE dt.a > 75;

-- if an alias is specified, don't allow the original table name
-- to be referenced
DELETE FROM delete_test_list dt WHERE dt.a > 25;

SELECT id, a, char_length(b) FROM delete_test_list order by 1, 2, 3;

-- delete a row with a TOASTed value
DELETE FROM delete_test_list WHERE a > 25;

SELECT id, a, char_length(b) FROM delete_test_list order by 1, 2, 3;

DROP TABLE delete_test_list;

-- section 2: 
create table hw_list_partition_dml_t1 (id int, name text)partition by list(id) (
partition hw_list_partition_dml_t1_p1 values(1,2,3,4,5,6,7,8,9),
partition hw_list_partition_dml_t1_p2 values(10,11,12,13,14,15,16,17,18,19),
partition hw_list_partition_dml_t1_p3 values(20,21,22,23,24,25,26,27,28,29));

create table hw_list_partition_dml_t2 (id int, name text)partition by list(id) (
partition hw_list_partition_dml_t2_p1 values(1,2,3,4,5,6,7,8,9),
partition hw_list_partition_dml_t2_p2 values(10,11,12,13,14,15,16,17,18,19),
partition hw_list_partition_dml_t2_p3 values(20,21,22,23,24,25,26,27,28,29));

create table hw_list_partition_dml_t3 (id int, name text)partition by list(id) (
partition hw_list_partition_dml_t3_p1 values(1,2,3,4,5,6,7,8,9),
partition hw_list_partition_dml_t3_p2 values(10,11,12,13,14,15,16,17,18,19),
partition hw_list_partition_dml_t3_p3 values(20,21,22,23,24,25,26,27,28,29));

-- section 2.1: two table join, both are partitioned table
insert into hw_list_partition_dml_t1 values (1, 'li'), (11, 'wang'), (21, 'zhang');
insert into hw_list_partition_dml_t2 values (1, 'xi'), (11, 'zhao'), (27, 'qi');
insert into hw_list_partition_dml_t3 values (1, 'qin'), (11, 'he'), (27, 'xiao');
-- delete 10~20 tupes in hw_partition_dml_t1
with T2_ID_10TH AS
(
SELECT id 
FROM hw_list_partition_dml_t2
WHERE id >= 10 and id < 20
ORDER BY id
)
delete from hw_list_partition_dml_t1
using hw_list_partition_dml_t2 
where hw_list_partition_dml_t1.id < hw_list_partition_dml_t2.id
	and hw_list_partition_dml_t2.id IN
		(SELECT id FROM T2_ID_10TH)
RETURNING hw_list_partition_dml_t1.name;
select * from hw_list_partition_dml_t1 order by 1, 2;
-- delete all tupes that is less than 11 in hw_list_partition_dml_t1, that is 3
insert into hw_list_partition_dml_t1 values (3, 'AAA'), (13, 'BBB'), (23, 'CCC'), (24, 'DDD');
select * from hw_list_partition_dml_t1 order by 1, 2;
delete from hw_list_partition_dml_t1 using hw_list_partition_dml_t2 where hw_list_partition_dml_t1.id < hw_list_partition_dml_t2.id and hw_list_partition_dml_t2.id = 11 RETURNING hw_list_partition_dml_t1.id;
select * from hw_list_partition_dml_t1 order by 1, 2;

-- section 2.2: delete from only one table, no joining
-- delete all tupes remaining: 13, 23, 24
delete from hw_list_partition_dml_t1;
select * from hw_list_partition_dml_t1 order by 1, 2;

-- section 3: 
-- section 3.1: two table join, only one is partitioned table
--              and target relation is partitioned
insert into hw_list_partition_dml_t1 values (1, 'AAA'), (11, 'BBB'), (21, 'CCC');
select * from hw_list_partition_dml_t1 order by 1, 2;
-- delete all tupes in hw_list_partition_dml_t1
delete from hw_list_partition_dml_t1 using hw_list_partition_dml_t3 where hw_list_partition_dml_t1.id < hw_list_partition_dml_t3.id and hw_list_partition_dml_t3.id = 27;
select * from hw_list_partition_dml_t1 order by 1, 2;
-- delete all tupes that is less than 11 in hw_list_partition_dml_t1, that is 3
insert into hw_list_partition_dml_t1 values (3, 'AAA'), (13, 'BBB'), (23, 'CCC'), (24, 'DDD');
select * from hw_list_partition_dml_t1 order by 1, 2;
delete from hw_list_partition_dml_t1 using hw_list_partition_dml_t3 where hw_list_partition_dml_t1.id < hw_list_partition_dml_t3.id and hw_list_partition_dml_t3.id = 11;
select * from hw_list_partition_dml_t1 order by 1, 2;

-- section 3.2 delete from only one table, no joining
-- delete all tupes remaining: 13, 23, 24
delete from hw_list_partition_dml_t1;
select * from hw_list_partition_dml_t1 order by 1, 2;

-- section 3.3: two table join, only one is partitioned table
--              and target relation is on-partitioned
-- delete all tuples in hw_list_partition_dml_t3
insert into hw_list_partition_dml_t2 values (28, 'EEE');
delete from hw_list_partition_dml_t3 using hw_list_partition_dml_t2 where hw_list_partition_dml_t3.id < hw_list_partition_dml_t2.id and hw_list_partition_dml_t2.id = 28;
select * from hw_list_partition_dml_t3 order by 1, 2;

-- delete all tuples that is less than 11 in hw_list_partition_dml_t3, that is 3
insert into hw_list_partition_dml_t3 values (3, 'AAA'), (13, 'BBB'), (23, 'CCC'), (24, 'DDD');
delete from hw_list_partition_dml_t3 using hw_list_partition_dml_t2 where hw_list_partition_dml_t3.id < hw_list_partition_dml_t2.id and hw_list_partition_dml_t2.id = 11;
select * from hw_list_partition_dml_t3 order by 1, 2;

-- section 3.4 delete from only one table, no joining
-- delete all tuples remaining: 13, 23, 24
delete from hw_list_partition_dml_t3;
select * from hw_list_partition_dml_t3 order by 1, 2;

-- finally, drop table hw_list_partition_dml_t1, hw_list_partition_dml_t2 and hw_list_partition_dml_t3
drop table hw_list_partition_dml_t1;
drop table hw_list_partition_dml_t2;
drop table hw_list_partition_dml_t3;
drop schema FVT_COMPRESS_QWER cascade;
create schema fvt_other_cmd;
CREATE TABLE FVT_OTHER_CMD.IDEX_LIST_PARTITION_TABLE_001(COL_INT int)
partition by list (COL_INT)
( 
     partition IDEX_LIST_PARTITION_TABLE_001_1 values (1000,2000),
     partition IDEX_LIST_PARTITION_TABLE_001_2 values (3000,4000,5000),
     partition IDEX_LIST_PARTITION_TABLE_001_3 values (6000,7000,8000,9000,10000)
);
declare  
i int; 
begin i:=1;  
while
i<9990 LOOP
Delete from FVT_OTHER_CMD.IDEX_LIST_PARTITION_TABLE_001 where col_int=i; 
i:=i+1000; 
end loop;      
end;
/

drop schema fvt_other_cmd cascade;
