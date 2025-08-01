#
# Copyright 2010-2011,2014-2015 Ettus Research LLC
# Copyright 2018 Ettus Research, a National Instruments Company
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

UHD_INSTALL(FILES

    #### dboard headers ###
    fe_connection.hpp
    dboard_base.hpp
    dboard_eeprom.hpp
    dboard_id.hpp
    dboard_iface.hpp
    dboard_manager.hpp
    zbx_tune_map_item.hpp

    ### utilities ###
    gpio_defs.hpp
    mboard_eeprom.hpp
    subdev_spec.hpp

    ### interfaces ###
    multi_usrp.hpp

    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uhd/usrp
    COMPONENT headers
)

if(ENABLE_C_API)
    UHD_INSTALL(FILES
        dboard_eeprom.h
        mboard_eeprom.h
        subdev_spec.h
        usrp.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uhd/usrp
        COMPONENT headers
    )
endif(ENABLE_C_API)
