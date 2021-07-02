alter node node_1 with (port=12000);
drop node node1;
create node group nodegroup1 with (dn1, dn2, dn3);
alter node group nodegroup1 rename to nodegroup2;
drop node group nodegroup1;
create role role1 with node group node_group1 password disable;
alter role role1 with node group node_group2;
create group group1 with node group node_group1 password disable;
create user user1 node group nodegroup1;
alter user user1 node group nodegroup1;
create table table1 (id int);
select /*+ redistribute(table1) +*/ * from table1;
drop table table1;
create barrier;
clean connection to node (dn2) for database regression;
execute direct on (dn2) 'select * from table1';
create table table_neg_tmp (id int);
create table table1 (id int) to node (dn2);
create table table2 (like table_neg_tmp including distribution);
drop table table_neg_tmp;

create node group group1_90 with (datanode);
alter node group group1_90 set default;
drop node group group1_90;