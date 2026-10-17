!ifdef MASTER_MAKE
BUILD_BITS=32
BUILD_TYPE=block
!INCLUDE $(DDKROOT)\master.mk
!endif

# Shared sources live one level up (src\); MMC_W9X selects the 9x target
INCLUDE =.;..;$(INCLUDE)

DEVICE          = mmcsd9x
OBJS            = mmcmain.obj mmccmd.obj mmcsatl.obj mmcacpi.obj

######### Definitions for the debug level #########

# DEBUG=1: compile in debug output
!ifdef DEBUG

DDEBUG =-DDEBLEVEL=1 -DDEBUG
CFLAGS =$(CFLAGS) -Oy1

!else

DDEBUG =-DDEBLEVEL=0

!endif

########## Definitions for C compiler #############

CL      = cl
CFLAGS  = -nologo -Zp4 -Gs -c -DIS_32 -D_X86_ -Zl -Gz -DDBG -DMMC_W9X $(CFLAGS)
CFLAGS  = $(CFLAGS) $(DDEBUG)

########## Definitions for linker #############

LINK 	 = link

########## Base inference rules ###################

#       32 bit C compiler inference rules

.c.obj:
	set CL=$(CFLAGS)
	$(CL) -Fo$*.obj $<

############### VxD device section ####################

$(DEVICE).sym: $(DEVICE).map
	mapsym -s -o .\$(DEVICE).sym .\$(DEVICE).map > nul

$(DEVICE).map: $(DEVICE).mpd

$(DEVICE).mpd : $(OBJS) $(DEVICE).res $(LIBS) $(STUB)
	$(LINK) @$(DEVICE).lnk

# Shared sources (after the default target above)

mmcmain.obj: ..\mmcmain.c ..\mmcsd.h
	set CL=$(CFLAGS)
	$(CL) -Fommcmain.obj ..\mmcmain.c

mmccmd.obj: ..\mmccmd.c ..\mmcsd.h
	set CL=$(CFLAGS)
	$(CL) -Fommccmd.obj ..\mmccmd.c

mmcsatl.obj: ..\mmcsatl.c ..\mmcsd.h
	set CL=$(CFLAGS)
	$(CL) -Fommcsatl.obj ..\mmcsatl.c

mmcacpi.obj: ..\mmcacpi.c ..\mmcsd.h
	set CL=$(CFLAGS)
	$(CL) -Fommcacpi.obj ..\mmcacpi.c

clean:
	-@del *.obj
	-@del *.mpd
	-@del *.map
	-@del *.pdb
	-@del *.sym

$(DEVICE).res: ..\mmcsdnt.rc
	rc -r -DMMC_W9X -fo$(DEVICE).res ..\mmcsdnt.rc
