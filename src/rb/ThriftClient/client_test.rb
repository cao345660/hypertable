#!/usr/bin/env ruby
require File.dirname(__FILE__) + '/hypertable/thrift_client'
require 'pp'
include Hypertable::ThriftGen;

begin
  Hypertable.with_thrift_client("localhost", 38080) do |client|
    puts "testing hql queries..."
    res = client.hql_query("drop table if exists thrift_test");
    res = client.hql_query("create table thrift_test ( col )");
    res = client.hql_query("insert into thrift_test values \
                           ('2008-11-11 11:11:11', 'k1', 'col', 'v1'), \
                           ('2008-11-11 11:11:11', 'k2', 'col', 'v2'), \
                           ('2008-11-11 11:11:11', 'k3', 'col', 'v3')");
    res = client.hql_query("select * from thrift_test");
    pp res

    puts "testing scanner api..."
    # testing scanner api
    client.with_scanner("thrift_test", ScanSpec.new()) do |scanner|
      client.each_cell(scanner) { |cell| pp cell }
    end

    puts "testing mutator api..."
    client.with_mutator("thrift_test") do |mutator|
      cell = Cell.new
      cell.row_key = "k4"
      cell.column_family = "col"
      cell.value = "v4"
      cell.timestamp = 1226401871000000000; # 2008-11-11 11:11:11
      client.set_cell(mutator, cell);
    end

    puts "checking for k4..."
    pp client.hql_query("select * from thrift_test where row > 'k3'")
  end
rescue
  pp $!
  exit 1
end
