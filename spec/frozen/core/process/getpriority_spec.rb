require File.dirname(__FILE__) + '/../../spec_helper'

describe "Process.getpriority" do
  not_supported_on :windows do
    it "takes two arguments" do
      lambda { Process.getpriority }.should raise_error(ArgumentError)
      lambda {
        Process.getpriority(Process::PRIO_PROCESS)
      }.should raise_error(ArgumentError)
    end
  
    it "gets the scheduling priority for a specified process" do
      Process.getpriority(Process::PRIO_PROCESS, 0).class.should == Fixnum
    end
  
    it "gets the scheduling priority for a specified process group" do
      Process.getpriority(Process::PRIO_PGRP, 0).class.should == Fixnum
    end
  
    it "gets the scheduling priority for a specified user" do
      Process.getpriority(Process::PRIO_USER, 0).class.should == Fixnum
    end
  end
end
