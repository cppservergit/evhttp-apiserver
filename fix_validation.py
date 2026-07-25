import re

def fix_validation():
    # 1. Update src/handlers.c
    with open("src/handlers.c", "r") as f:
        s = f.read()

    # customer_id_validator
    s = s.replace(
        'return emit_error(err_buf, err_len, ERR_INVALID_CUSTOMER_ID, id ? id : "null");',
        'snprintf(err_buf, err_len, "Invalid customer ID format: %s", id ? id : "null");\n        return false;'
    )
    s = s.replace(
        'return emit_error(err_buf, err_len, ERR_INVALID_CUSTOMER_ID, id);',
        'snprintf(err_buf, err_len, "Invalid customer ID character: %s", id);\n            return false;'
    )

    # sales_invariant_validator
    s = s.replace(
        'return emit_error(err_buf, err_len, ERR_START_AFTER_END, nullptr);',
        'snprintf(err_buf, err_len, "Start date must strictly precede end date");\n        return false;'
    )

    # validate_sales_start_date
    s = s.replace(
        'return emit_error(err_buf, err_len, ERR_DATE_TOO_EARLY, nullptr);',
        'snprintf(err_buf, err_len, "Start date is too early (min 1994)");\n        return false;'
    )

    # validate_sales_end_date
    s = s.replace(
        'return emit_error(err_buf, err_len, ERR_DATE_TOO_LATE, nullptr);',
        'snprintf(err_buf, err_len, "End date is too late (max 1996)");\n        return false;'
    )

    with open("src/handlers.c", "w") as f:
        f.write(s)

    # 2. Update include/validation.h
    with open("include/validation.h", "r") as f:
        v_h = f.read()

    v_h = v_h.replace("    ERR_NEGATIVE_AMOUNT,\n", "")
    v_h = v_h.replace("    ERR_START_AFTER_END,\n", "")
    v_h = v_h.replace("    ERR_INVALID_CUSTOMER_ID,\n", "")
    v_h = v_h.replace("    ERR_DATE_TOO_EARLY,\n", "")
    v_h = v_h.replace("    ERR_DATE_TOO_LATE,\n", "")

    with open("include/validation.h", "w") as f:
        f.write(v_h)

    # 3. Update src/validation.c (Remove specific lines)
    with open("src/validation.c", "r") as f:
        v_c = f.read()

    v_c = re.sub(r'\s*case ERR_NEGATIVE_AMOUNT:\s*snprintf\(.*?\);\s*break;', '', v_c)
    v_c = re.sub(r'\s*case ERR_START_AFTER_END:\s*snprintf\(.*?\);\s*break;', '', v_c)
    v_c = re.sub(r'\s*case ERR_INVALID_CUSTOMER_ID:\s*snprintf\(.*?\);\s*break;', '', v_c)
    v_c = re.sub(r'\s*case ERR_DATE_TOO_EARLY:\s*snprintf\(.*?\);\s*break;', '', v_c)
    v_c = re.sub(r'\s*case ERR_DATE_TOO_LATE:\s*snprintf\(.*?\);\s*break;', '', v_c)

    with open("src/validation.c", "w") as f:
        f.write(v_c)

if __name__ == "__main__":
    fix_validation()
