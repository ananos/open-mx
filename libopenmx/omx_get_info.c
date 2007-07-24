#include <errno.h>
#include <sys/ioctl.h>

#include "omx_io.h"
#include "omx__lib.h"

/*
 * Returns the current amount of boards attached to the driver
 */
omx_return_t
omx__get_board_count(uint32_t * count)
{
  omx_return_t ret = OMX_SUCCESS;
  int err;

  if (!omx__globals.initialized) {
    ret = OMX_NOT_INITIALIZED;
    goto out;
  }

  err = ioctl(omx__globals.control_fd, OMX_CMD_GET_BOARD_COUNT, count);
  if (err < 0) {
    ret = omx__errno_to_return(errno, "ioctl GET_BOARD_COUNT");
    goto out;
  }

 out:
  return ret;
}

/*
 * Returns the board id of the endpoint is non NULL,
 * or the current board corresponding to the index.
 *
 * index, name and addr pointers may be NULL is unused.
 */
omx_return_t
omx__get_board_id(struct omx_endpoint * ep, uint8_t * index,
		  char * name, uint64_t * addr)
{
  omx_return_t ret = OMX_SUCCESS;
  struct omx_cmd_get_board_id board_id;
  int err, fd;

  if (!omx__globals.initialized) {
    ret = OMX_NOT_INITIALIZED;
    goto out;
  }

  if (ep) {
    /* use the endpoint fd */
    fd = ep->fd;
  } else {
    /* use the control fd and the index */
    fd = omx__globals.control_fd;
    board_id.board_index = *index;
  }

  err = ioctl(fd, OMX_CMD_GET_BOARD_ID, &board_id);
  if (err < 0) {
    ret = omx__errno_to_return(errno, "ioctl GET_BOARD_ID");
    goto out;
  }

  if (name)
    strncpy(name, board_id.board_name, OMX_HOSTNAMELEN_MAX);
  if (index)
    *index = board_id.board_index;
  if (addr)
    *addr = board_id.board_addr;

 out:
  return ret;
}

/*
 * Returns the current index of a board given by its name
 */
omx_return_t
omx__get_board_index_by_name(const char * name, uint8_t * index)
{
  omx_return_t ret = OMX_SUCCESS;
  uint32_t max;
  int err, i;

  if (!omx__globals.initialized) {
    ret = OMX_NOT_INITIALIZED;
    goto out;
  }

  err = ioctl(omx__globals.control_fd, OMX_CMD_GET_BOARD_MAX, &max);
  if (err < 0) {
    ret = omx__errno_to_return(errno, "ioctl GET_BOARD_MAX");
    goto out;
  }

  ret = OMX_INVALID_PARAMETER;
  for(i=0; i<max; i++) {
    struct omx_cmd_get_board_id board_id;

    board_id.board_index = i;
    err = ioctl(omx__globals.control_fd, OMX_CMD_GET_BOARD_ID, &board_id);
    if (err < 0) {
      ret = omx__errno_to_return(errno, "ioctl GET_BOARD_ID");
      if (ret != OMX_INVALID_PARAMETER)
	goto out;
    }

    if (!strncmp(name, board_id.board_name, OMX_HOSTNAMELEN_MAX)) {
      ret = OMX_SUCCESS;
      *index = i;
      break;
    }
  }

 out:
  return ret;
}

/*
 * Returns the current index of a board given by its addr
 */
omx_return_t
omx__get_board_index_by_addr(uint64_t addr, uint8_t * index)
{
  omx_return_t ret = OMX_SUCCESS;
  uint32_t max;
  int err, i;

  if (!omx__globals.initialized) {
    ret = OMX_NOT_INITIALIZED;
    goto out;
  }

  err = ioctl(omx__globals.control_fd, OMX_CMD_GET_BOARD_MAX, &max);
  if (err < 0) {
    ret = omx__errno_to_return(errno, "ioctl GET_BOARD_MAX");
    goto out;
  }

  ret = OMX_INVALID_PARAMETER;
  for(i=0; i<max; i++) {
    struct omx_cmd_get_board_id board_id;

    board_id.board_index = i;
    err = ioctl(omx__globals.control_fd, OMX_CMD_GET_BOARD_ID, &board_id);
    if (err < 0) {
      ret = omx__errno_to_return(errno, "ioctl GET_BOARD_ID");
      if (ret != OMX_INVALID_PARAMETER)
	goto out;
    }

    if (addr == board_id.board_addr) {
      ret = OMX_SUCCESS;
      *index = i;
      break;
    }
  }

 out:
  return ret;
}

/*
 * Returns various info
 */
omx_return_t
omx_get_info(struct omx_endpoint * ep, enum omx_info_key key,
	     const void * in_val, uint32_t in_len,
	     void * out_val, uint32_t out_len)
{
  switch (key) {
  case OMX_INFO_BOARD_MAX:
    if (!omx__globals.initialized)
      return OMX_NOT_INITIALIZED;
    if (out_len < sizeof(uint32_t))
      return OMX_INVALID_PARAMETER;
    *(uint32_t *) out_val = omx__globals.board_max;
    return OMX_SUCCESS;

  case OMX_INFO_ENDPOINT_MAX:
    if (!omx__globals.initialized)
      return OMX_NOT_INITIALIZED;
    if (out_len < sizeof(uint32_t))
      return OMX_INVALID_PARAMETER;
    *(uint32_t *) out_val = omx__globals.endpoint_max;
    return OMX_SUCCESS;

  case OMX_INFO_BOARD_COUNT:
    if (out_len < sizeof(uint32_t))
      return OMX_INVALID_PARAMETER;
    return omx__get_board_count((uint32_t *) out_val);

  case OMX_INFO_BOARD_NAME:
    if (ep) {
      /* use the info stored in the endpoint */
      strncpy(out_val, ep->board_name, out_len);
      return OMX_SUCCESS;

    } else {
      /* if no endpoint given, ask the driver about the index given in in_val */
      uint64_t addr;
      char name[OMX_HOSTNAMELEN_MAX];
      uint8_t index;
      omx_return_t ret;

      if (!in_val || !in_len)
	return OMX_INVALID_PARAMETER;
      index = *(uint8_t*)in_val;

      ret = omx__get_board_id(ep, &index, name, &addr);
      if (ret != OMX_SUCCESS)
	return ret;

      strncpy(out_val, name, out_len);
    }

  case OMX_INFO_BOARD_INDEX_BY_NAME:
    if (!out_val || !out_len)
      return OMX_INVALID_PARAMETER;
    if (ep) {
      /* use the info stored in the endpoint */
      *(uint8_t*) out_val = ep->board_index;
      return OMX_SUCCESS;

    } else {
      return omx__get_board_index_by_name(in_val, out_val);
    }

  default:
    return OMX_INVALID_PARAMETER;
  }

  return OMX_SUCCESS;
}

/*
 * Translate local board number/addr
 */

omx_return_t
omx_board_number_to_nic_id(uint32_t board_number,
			   uint64_t *nic_id)
{
  uint8_t index = board_number;
  return omx__get_board_id(NULL, &index, NULL, nic_id);
}

omx_return_t
omx_nic_id_to_board_number(uint64_t nic_id,
			   uint32_t *board_number)
{
  omx_return_t ret;
  uint8_t index;

  ret = omx__get_board_index_by_addr(nic_id, &index);
  if (ret == OMX_SUCCESS)
    *board_number = index;
  return ret;
}
