CREATE schema FVT_COMPRESS_QWER;
set search_path to FVT_COMPRESS_QWER;
create table bmsql_order_line (
  ol_w_id         integer   not null,
  ol_d_id         integer   not null,
  ol_o_id         integer   not null,
  ol_number       integer   not null,
  ol_i_id         integer   not null,
  ol_delivery_d   timestamp,
  ol_amount       decimal(6,2),
  ol_supply_w_id  integer,
  ol_quantity     integer,
  ol_dist_info    char(24)
)
partition by list(ol_d_id)
(
  partition p0 values (1,4,7),
  partition p1 values (2,5,8),
  partition p2 values (3,6,9)
);
alter table bmsql_order_line add constraint bmsql_order_line_pkey primary key (ol_w_id, ol_d_id, ol_o_id, ol_number);
insert into bmsql_order_line(ol_w_id, ol_d_id, ol_o_id, ol_number, ol_i_id, ol_dist_info) values(1, 1, 1, 1, 1, '123');
update bmsql_order_line set ol_dist_info='ss' where ol_w_id =1;
delete from bmsql_order_line;

create table test_partition_for_null_list_timestamp
(
	a timestamp without time zone,
	b timestamp with time zone,
	c int,
	d int) 
partition by list (a) 
(
	partition test_partition_for_null_list_timestamp_p1 values ('2000-01-01 01:01:01', '2000-01-01 01:01:02'),
	partition test_partition_for_null_list_timestamp_p2 values ('2000-02-02 02:02:02', '2000-02-02 02:02:04'),
	partition test_partition_for_null_list_timestamp_p3 values ('2000-03-03 03:03:03', '2000-03-03 03:03:06')
);
create index idx_test_partition_for_null_list_timestamp_1 on test_partition_for_null_list_timestamp(a) LOCAL;
create index idx_test_partition_for_null_list_timestamp_2 on test_partition_for_null_list_timestamp(a,b) LOCAL;
create index idx_test_partition_for_null_list_timestamp_3 on test_partition_for_null_list_timestamp(c) LOCAL;
create index idx_test_partition_for_null_list_timestamp_4 on test_partition_for_null_list_timestamp(b,c,d) LOCAL;

create table test_partition_for_null_list_text (a text, b varchar(2), c char(1), d varchar(2)) 
partition by list (a) 
(
	partition test_partition_for_null_list_text_p1 values ('A'),
	partition test_partition_for_null_list_text_p2 values ('B','C','D','E'),
	partition test_partition_for_null_list_text_p3 values ('F','G')
);
create index idx_test_partition_for_null_list_text_1 on test_partition_for_null_list_text(a) LOCAL;
create index idx_test_partition_for_null_list_text_2 on test_partition_for_null_list_text(a,b) LOCAL;
create index idx_test_partition_for_null_list_text_3 on test_partition_for_null_list_text(c) LOCAL;
create index idx_test_partition_for_null_list_text_4 on test_partition_for_null_list_text(b,c,d) LOCAL;
create index idx_test_partition_for_null_list_text_5 on test_partition_for_null_list_text(b,c,d);

CREATE TABLE select_partition_table_000_1(
	C_CHAR_1 CHAR(1),
	C_CHAR_2 CHAR(10),
	C_CHAR_3 CHAR(102400),
	C_VARCHAR_1 VARCHAR(1),
	C_VARCHAR_2 VARCHAR(10),
	C_VARCHAR_3 VARCHAR(1024),
	C_INT INTEGER,
	C_BIGINT BIGINT,
	C_SMALLINT SMALLINT,
	C_FLOAT FLOAT,
	C_NUMERIC numeric(10,5),
	C_DP double precision,
	C_DATE DATE,
	C_TS_WITHOUT TIMESTAMP WITHOUT TIME ZONE,
	C_TS_WITH TIMESTAMP WITH TIME ZONE ) 
	partition by list (C_BIGINT)
( 
     partition select_partition_000_1_1 values (1,2,3,4),
     partition select_partition_000_1_2 values (5,6,7,8,9)
);
create index idx_select_partition_table_000_1_1 on select_partition_table_000_1(C_CHAR_1) LOCAL;
create index idx_select_partition_table_000_1_2 on select_partition_table_000_1(C_CHAR_1,C_VARCHAR_1) LOCAL;
create index idx_select_partition_table_000_1_3 on select_partition_table_000_1(C_BIGINT) LOCAL;
create index idx_select_partition_table_000_1_4 on select_partition_table_000_1(C_BIGINT,C_TS_WITH,C_DP) LOCAL;
create index idx_select_partition_table_000_1_5 on select_partition_table_000_1(C_BIGINT,C_NUMERIC,C_TS_WITHOUT);

CREATE TABLE select_partition_table_000_2(
	C_CHAR_1 CHAR(1),
	C_CHAR_2 CHAR(10),
	C_CHAR_3 CHAR(102400),
	C_VARCHAR_1 VARCHAR(1),
	C_VARCHAR_2 VARCHAR(10),
	C_VARCHAR_3 VARCHAR(1024),
	C_INT INTEGER,
	C_BIGINT BIGINT,
	C_SMALLINT SMALLINT,
	C_FLOAT FLOAT,
	C_NUMERIC numeric(10,5),
	C_DP double precision,
	C_DATE DATE,
	C_TS_WITHOUT TIMESTAMP WITHOUT TIME ZONE,
	C_TS_WITH TIMESTAMP WITH TIME ZONE ) 
	partition by list (C_SMALLINT)
( 
     partition select_partition_000_2_1 values (1,2,3,4),
     partition select_partition_000_2_2 values (5,6,7,8,9)
);
create index idx_select_partition_table_000_2_1 on select_partition_table_000_2(C_CHAR_2) LOCAL;
create index idx_select_partition_table_000_2_2 on select_partition_table_000_2(C_CHAR_2,C_VARCHAR_2) LOCAL;
create index idx_select_partition_table_000_2_3 on select_partition_table_000_2(C_SMALLINT) LOCAL;
create index idx_select_partition_table_000_2_4 on select_partition_table_000_2(C_SMALLINT,C_TS_WITH,C_DP) LOCAL;
create index idx_select_partition_table_000_2_5 on select_partition_table_000_2(C_SMALLINT,C_NUMERIC,C_TS_WITHOUT);

CREATE TABLE select_partition_table_000_3(
	C_CHAR_1 CHAR(1),
	C_CHAR_2 CHAR(10),
	C_CHAR_3 CHAR(102400),
	C_VARCHAR_1 VARCHAR(1),
	C_VARCHAR_2 VARCHAR(10),
	C_VARCHAR_3 VARCHAR(1024),
	C_INT INTEGER,
	C_BIGINT BIGINT,
	C_SMALLINT SMALLINT,
	C_FLOAT FLOAT,
	C_NUMERIC numeric(10,5),
	C_DP double precision,
	C_DATE DATE,
	C_TS_WITHOUT TIMESTAMP WITHOUT TIME ZONE,
	C_TS_WITH TIMESTAMP WITH TIME ZONE ) 
	partition by list (C_NUMERIC)
( 
     partition select_partition_000_3_1 values (1,2,3,4),
     partition select_partition_000_3_2 values (5,6,7,8,9)
);
CREATE TABLE select_partition_table_000_4(
	C_CHAR_1 CHAR(1),
	C_CHAR_2 CHAR(10),
	C_CHAR_3 CHAR(102400),
	C_VARCHAR_1 VARCHAR(1),
	C_VARCHAR_2 VARCHAR(10),
	C_VARCHAR_3 VARCHAR(1024),
	C_INT INTEGER,
	C_BIGINT BIGINT,
	C_SMALLINT SMALLINT,
	C_FLOAT FLOAT,
	C_NUMERIC numeric(10,5),
	C_DP double precision,
	C_DATE DATE,
	C_TS_WITHOUT TIMESTAMP WITHOUT TIME ZONE,
	C_TS_WITH TIMESTAMP WITH TIME ZONE ) 
	partition by list (C_DP)
( 
     partition select_partition_000_4_1 values (1,2,3,4),
     partition select_partition_000_4_2 values (5,6,7,8,9)
);
drop schema FVT_COMPRESS_QWER cascade;