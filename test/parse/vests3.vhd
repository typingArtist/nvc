
-- Copyright (C) 2001 Bill Billowitch.

-- Some of the work to develop this test suite was done with Air Force
-- support.  The Air Force and Bill Billowitch assume no
-- responsibilities for this software.

-- This file is part of VESTs (Vhdl tESTs).

-- VESTs is free software; you can redistribute it and/or modify it
-- under the terms of the GNU General Public License as published by the
-- Free Software Foundation; either version 2 of the License, or (at
-- your option) any later version.

-- VESTs is distributed in the hope that it will be useful, but WITHOUT
-- ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
-- FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
-- for more details.

-- You should have received a copy of the GNU General Public License
-- along with VESTs; if not, write to the Free Software Foundation,
-- Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

-- ---------------------------------------------------------------------
--
-- $Id: tc2858.vhd,v 1.2 2001-10-26 16:30:23 paw Exp $
-- $Revision: 1.2 $
--
-- ---------------------------------------------------------------------

ENTITY c13s10b00x00p04n01i02858ent IS
END c13s10b00x00p04n01i02858ent;

ARCHITECTURE c13s10b00x00p04n01i02858arch OF c13s10b00x00p04n01i02858ent IS

BEGIN
  TESTING: PROCESS
    variable bit_str : bit_vector(0 to 7) := "01010101%;
  BEGIN
    assert FALSE
      report "***FAILED TEST: c13s10b00x00p04n01i02858 - Only right hand side quotation mark ("") is replaced by percent character (%)."
      severity ERROR;
    wait;
  END PROCESS TESTING;

END c13s10b00x00p04n01i02858arch;
