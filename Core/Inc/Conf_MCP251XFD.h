/**
  ******************************************************************************
  * @file    Conf_MCP251XFD.h
  * @brief   MCP251XFD 드라이버 프로젝트 설정
  ******************************************************************************
  */
#ifndef CONF_MCP251XFD_H
#define CONF_MCP251XFD_H

#define MCP251XFD_TRANS_BUF_SIZE  18

_Static_assert(((MCP251XFD_TRANS_BUF_SIZE - 2) % 4) == 0,
               "TRANS_BUF_SIZE-2 must be a multiple of 4: MCP2518FD RAM is "
               "word-addressed and the driver only aligns in the CRC path");

#endif /* CONF_MCP251XFD_H */
