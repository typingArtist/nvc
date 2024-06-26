package vhdl5 is
    procedure get_it (x : out integer);
end package;

package body vhdl5 is
    type pt is protected
        impure function something return integer;
    end protected;

    type pt is protected body
        variable v : integer := 55;
        impure function something return integer is
        begin
            return v;
        end function;
    end protected body;

    shared variable sv : pt;

    procedure get_it (x : out integer) is
    begin
        x := sv.something;
    end procedure;

    procedure attrs is
        attribute foo : string;
        attribute foo of attrs [] : procedure is "123";
        attribute foo of all : literal is "abcd";
        attribute foo of others : procedure is "xxx";
    begin
    end procedure;

end package body;
